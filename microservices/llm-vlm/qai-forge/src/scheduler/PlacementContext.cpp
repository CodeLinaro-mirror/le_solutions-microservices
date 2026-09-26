// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/scheduler/PlacementContext.h"

#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/resources/QnnTopologyProbe.h"
#include "qai_forge/resources/StandardTopologies.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <utility>

namespace scheduler {

namespace {

std::shared_ptr<const qai_forge::resources::TopologyRegistry> requireTopology(
    std::shared_ptr<const qai_forge::resources::TopologyRegistry> topology) {
    if (!topology) {
        throw std::invalid_argument(
            "PlacementContext requires a topology registry");
    }
    return topology;
}

std::shared_ptr<const qai_forge::resources::TopologyRegistry>
topologyFromEnvironment() {
    const char* platform = std::getenv("QAI_FORGE_PLATFORM");
    const std::string requested = platform ? platform : "";

    std::unique_ptr<qai_forge::resources::TopologyRegistry> registry;
    if (requested == "iq9" || requested == "IQ9" ||
        requested == "IQ9-QCS9075") {
        registry = qai_forge::resources::TopologyRegistry::create(
            qai_forge::resources::makeIQ9Topology());
    } else if (requested == "ventunoq" || requested == "VentunoQ") {
        registry = qai_forge::resources::TopologyRegistry::create(
            qai_forge::resources::makeVentunoQTopology());
    } else {
        const char* backend_lib = std::getenv("QNN_BACKEND_LIB");
        if (backend_lib && backend_lib[0] != '\0') {
            const auto probe =
                qai_forge::resources::QnnTopologyProbe::probe(backend_lib);
            if (probe.succeeded && !probe.topology.nodes.empty()) {
                registry = qai_forge::resources::TopologyRegistry::create(
                    probe.topology);
            }
        }

        if (!registry) {
            registry = qai_forge::resources::TopologyRegistry::createUnknown();
        }
    }

    return std::shared_ptr<const qai_forge::resources::TopologyRegistry>(
        std::move(registry));
}

std::string providerForModel(const std::string& model_id) {
    const std::string runtime =
        ModelConfigManager::getInstance().getRuntime(model_id);

    if (runtime == "qnn" || runtime == "qnn_context_binary") {
        return "qnn";
    }
    if (runtime == "snpe" || runtime == "snpe_dlc" ||
        runtime == "qnn_dlc") {
        return "snpe";
    }
    if (runtime == "litert" || runtime == "tflite") {
        return "litert";
    }
    if (runtime == "litert_lm") {
        return "litert_lm";
    }
    if (runtime == "llamacpp") {
        return "llamacpp";
    }

    return "genie";
}

bool usesAcceleratorDomain(const std::string& provider) {
    return provider == "qnn" || provider == "genie" ||
           provider == "llamacpp" || provider == "snpe" ||
           provider == "litert" || provider == "litert_lm";
}

std::string snpeRuntimeOrderFromEnvironment() {
    const char* delegate = std::getenv("SNPE_DELEGATE");
    const std::string requested = delegate ? delegate : "dsp";

    if (requested == "dsp") {
        return "DSP,CPU";
    }
    if (requested == "aip") {
        return "AIP_FIXED8_TF,CPU";
    }
    if (requested == "gpu") {
        return "GPU,CPU";
    }
    if (requested == "none" || requested == "cpu") {
        return "CPU";
    }

    return "DSP,CPU";
}

uint32_t sharedUsersFor(
    const qai_forge::resources::ResourceSnapshot& snapshot,
    const qai_forge::resources::ResourceId& resource) {
    for (const auto& entry : snapshot.resources) {
        if (entry.resource == resource) {
            return entry.shared_users;
        }
    }
    return 0;
}

}  // namespace

std::shared_ptr<PlacementContext> PlacementContext::createDefault() {
    auto topology = topologyFromEnvironment();
    return std::make_shared<PlacementContext>(
        std::move(topology),
        qai_forge::execution::ProviderRegistry::createDefault());
}

PlacementContext::PlacementContext(
    std::shared_ptr<const qai_forge::resources::TopologyRegistry> topology,
    qai_forge::execution::ProviderRegistry providers)
    : topology_(requireTopology(std::move(topology))),
      providers_(std::move(providers)),
      resource_manager_(
          std::make_shared<qai_forge::resources::ResourceManager>(topology_)),
      resolver_(topology_, providers_) {}

PlacementAdmissionResult PlacementContext::tryAcquire(
    const qai_forge::execution::RuntimeId& runtime_id,
    const std::string& model_id) const {
    return tryAcquirePlan(runtime_id, model_id, defaultPlan(model_id));
}

PlacementAdmissionResult PlacementContext::tryAcquireAutomatic(
    const qai_forge::execution::RuntimeId& runtime_id,
    const std::string& model_id) const {
    PlacementAdmissionResult result;
    if (runtime_id.model_id.empty() || model_id.empty() ||
        runtime_id.model_id != model_id) {
        result.denial_reason =
            "Automatic placement requires matching non-empty runtime and model IDs";
        return result;
    }

    const std::string provider = providerForModel(model_id);
    if (provider != "qnn" && provider != "genie") {
        return tryAcquirePlan(runtime_id, model_id, defaultPlan(model_id));
    }

    struct Candidate {
        uint32_t device_id = 0;
        uint32_t shared_users = 0;
        std::string resource_id;
    };
    std::vector<Candidate> candidates;
    const auto snapshot = resource_manager_->snapshot();

    for (const auto* node :
         topology_->byKind(qai_forge::resources::ResourceKind::Htp)) {
        if (!node) {
            continue;
        }

        const auto native_id = node->native_ids.find(provider);
        if (native_id == node->native_ids.end()) {
            continue;
        }

        try {
            size_t consumed = 0;
            const unsigned long long parsed =
                std::stoull(native_id->second, &consumed, 10);
            if (consumed != native_id->second.size() ||
                parsed > std::numeric_limits<uint32_t>::max()) {
                continue;
            }

            const qai_forge::resources::ResourceId resource = node->id;
            const bool resource_unavailable = std::any_of(
                snapshot.resources.begin(), snapshot.resources.end(),
                [&resource](const auto& entry) {
                    return entry.resource == resource && entry.unavailable;
                });
            const bool parent_unavailable =
                node->parent.has_value() &&
                std::any_of(
                    snapshot.resources.begin(),
                    snapshot.resources.end(),
                    [&parent = node->parent.value()](const auto& entry) {
                        return entry.resource == parent && entry.unavailable;
                    });
            if (resource_unavailable || parent_unavailable) {
                continue;
            }

            candidates.push_back(
                {static_cast<uint32_t>(parsed),
                 sharedUsersFor(snapshot, resource),
                 resource.value});
        } catch (...) {
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& lhs, const Candidate& rhs) {
                  if (lhs.shared_users != rhs.shared_users) {
                      return lhs.shared_users < rhs.shared_users;
                  }
                  if (lhs.device_id != rhs.device_id) {
                      return lhs.device_id < rhs.device_id;
                  }
                  return lhs.resource_id < rhs.resource_id;
              });

    if (candidates.empty()) {
        result.denial_reason =
            "Automatic " + provider +
            " placement found no healthy mapped HTP candidates";
        return result;
    }

    std::string last_denial;
    for (const auto& candidate : candidates) {
        PlacementAdmissionResult candidate_result;
        if (provider == "qnn") {
            candidate_result = tryAcquireQnnDevice(
                runtime_id, model_id, candidate.device_id);
        } else {
            candidate_result = tryAcquirePlan(
                runtime_id,
                model_id,
                geniePreferredPlan(candidate.device_id));
        }
        if (candidate_result.granted) {
            return candidate_result;
        }
        last_denial = candidate_result.denial_reason;
    }

    result.denial_reason =
        "Automatic " + provider + " placement could not acquire a candidate";
    if (!last_denial.empty()) {
        result.denial_reason += ": " + last_denial;
    }
    return result;
}

PlacementAdmissionResult PlacementContext::tryAcquireQnnDevice(
    const qai_forge::execution::RuntimeId& runtime_id,
    const std::string& model_id,
    uint32_t qnn_device_id) const {
    if (providerForModel(model_id) != "qnn") {
        PlacementAdmissionResult result;
        result.denial_reason =
            "QNN placement requested for non-QNN model '" + model_id + "'";
        return result;
    }

    return tryAcquirePlan(
        runtime_id, model_id, qnnExactPlan(qnn_device_id));
}

PlacementAdmissionResult PlacementContext::tryAcquirePlan(
    const qai_forge::execution::RuntimeId& runtime_id,
    const std::string& model_id,
    const qai_forge::execution::ExecutionPlan& plan) const {
    PlacementAdmissionResult result;
    if (runtime_id.model_id.empty() || model_id.empty() ||
        runtime_id.model_id != model_id) {
        result.denial_reason =
            "Placement admission requires matching non-empty runtime and model IDs";
        return result;
    }

    const auto validation = resolver_.resolve(plan);
    if (!validation.valid) {
        result.denial_reason = validation.error;
        return result;
    }

    auto lease_result =
        resource_manager_->tryAcquire(runtime_id,
                                      validation.normalized_plan.allClaims());
    if (!lease_result.granted) {
        result.denial_reason = lease_result.denial_reason;
        return result;
    }

    result.granted = true;
    result.reservation = PlacementReservation(
        validation.normalized_plan, std::move(lease_result.lease));
    return result;
}

qai_forge::execution::ExecutionPlan PlacementContext::geniePreferredPlan(
    uint32_t genie_device_id) const {
    qai_forge::execution::ExecutionPlan plan;
    plan.provider = "genie";

    qai_forge::execution::ComponentPlacement component;
    component.role = qai_forge::execution::ComponentRole::MainGraph;
    component.target.provider = "genie";
    component.target.target_id =
        "device_id=" + std::to_string(genie_device_id);
    component.target.semantics =
        qai_forge::execution::TargetSemantics::PhysicalInstance;
    component.target.discovery_mode =
        qai_forge::execution::DiscoveryMode::Configured;
    component.target.binding_strength =
        qai_forge::execution::BindingStrength::Preferred;

    const qai_forge::resources::ResourceNode* selected_node = nullptr;
    for (const auto* node :
         topology_->byKind(qai_forge::resources::ResourceKind::Htp)) {
        if (!node) {
            continue;
        }
        const auto native_id = node->native_ids.find("genie");
        if (native_id != node->native_ids.end() &&
            native_id->second == std::to_string(genie_device_id)) {
            selected_node = node;
            break;
        }
    }

    const qai_forge::resources::ResourceId selected_resource =
        selected_node
            ? selected_node->id
            : qai_forge::resources::ResourceId(
                  "htp:" + std::to_string(genie_device_id));

    component.claims.push_back({
        selected_resource,
        1,
        qai_forge::execution::ClaimRequirement::Required,
        qai_forge::execution::ResourceClaimMode::Shared});

    if (selected_node && selected_node->parent.has_value()) {
        component.claims.push_back({
            selected_node->parent.value(),
            1,
            qai_forge::execution::ClaimRequirement::Required,
            qai_forge::execution::ResourceClaimMode::Shared});
    }

    plan.components.push_back(std::move(component));
    return plan;
}

qai_forge::execution::ExecutionPlan PlacementContext::qnnExactPlan(
    uint32_t qnn_device_id) const {
    qai_forge::execution::ExecutionPlan plan;
    plan.provider = "qnn";

    qai_forge::execution::ComponentPlacement component;
    component.role = qai_forge::execution::ComponentRole::MainGraph;
    component.target.provider = "qnn";
    component.target.target_id =
        "device_id=" + std::to_string(qnn_device_id);
    component.target.semantics =
        qai_forge::execution::TargetSemantics::PhysicalInstance;
    component.target.discovery_mode =
        qai_forge::execution::DiscoveryMode::Enumerated;
    component.target.binding_strength =
        qai_forge::execution::BindingStrength::Exact;

    const qai_forge::resources::ResourceNode* selected_node = nullptr;
    for (const auto* node :
         topology_->byKind(qai_forge::resources::ResourceKind::Htp)) {
        if (!node) {
            continue;
        }

        const auto native_id = node->native_ids.find("qnn");
        if (native_id != node->native_ids.end() &&
            native_id->second == std::to_string(qnn_device_id)) {
            selected_node = node;
            break;
        }
    }

    const qai_forge::resources::ResourceId selected_resource =
        selected_node
            ? selected_node->id
            : qai_forge::resources::ResourceId(
                  "htp:" + std::to_string(qnn_device_id));

    component.claims.push_back({
        selected_resource,
        1,
        qai_forge::execution::ClaimRequirement::Required,
        qai_forge::execution::ResourceClaimMode::Shared});

    if (selected_node && selected_node->parent.has_value()) {
        component.claims.push_back({
            selected_node->parent.value(),
            1,
            qai_forge::execution::ClaimRequirement::Required,
            qai_forge::execution::ResourceClaimMode::Shared});
    }

    plan.components.push_back(std::move(component));
    return plan;
}

qai_forge::execution::ExecutionPlan PlacementContext::defaultPlan(
    const std::string& model_id) const {
    qai_forge::execution::ExecutionPlan plan;
    plan.provider = providerForModel(model_id);

    qai_forge::execution::ComponentPlacement component;
    component.role = qai_forge::execution::ComponentRole::MainGraph;
    component.target.provider = plan.provider;
    component.target.semantics =
        plan.provider == "snpe"
            ? qai_forge::execution::TargetSemantics::RuntimePreference
            : qai_forge::execution::TargetSemantics::BackendAutomatic;
    component.target.discovery_mode =
        plan.provider == "snpe"
            ? qai_forge::execution::DiscoveryMode::Configured
            : qai_forge::execution::DiscoveryMode::BackendManaged;
    component.target.binding_strength =
        plan.provider == "snpe"
            ? qai_forge::execution::BindingStrength::Preferred
            : qai_forge::execution::BindingStrength::Automatic;

    if (plan.provider == "snpe") {
        component.target.target_id =
            "runtime_order=" + snpeRuntimeOrderFromEnvironment();
    } else if (plan.provider == "litert_lm") {
        const char* env = std::getenv("LITERT_LM_BACKEND");
        const std::string backend = (env && env[0] != '\0') ? env : "npu";
        component.target.semantics =
            qai_forge::execution::TargetSemantics::BackendAutomatic;
        component.target.discovery_mode =
            qai_forge::execution::DiscoveryMode::Configured;
        component.target.binding_strength =
            qai_forge::execution::BindingStrength::Preferred;
        component.target.target_id = "backend=" + backend;
    }

    if (usesAcceleratorDomain(plan.provider) &&
        topology_->find(qai_forge::resources::ResourceId("cdsp:0")) !=
            nullptr) {
        component.claims.push_back({
            qai_forge::resources::ResourceId("cdsp:0"),
            1,
            qai_forge::execution::ClaimRequirement::Required,
            qai_forge::execution::ResourceClaimMode::Shared});
    }

    plan.components.push_back(std::move(component));
    return plan;
}

}  // namespace scheduler
