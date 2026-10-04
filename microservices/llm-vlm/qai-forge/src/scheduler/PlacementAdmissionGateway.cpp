// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/scheduler/PlacementAdmissionGateway.h"

#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/resources/QnnTopologyProbe.h"
#include "qai_forge/resources/StandardTopologies.h"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace scheduler {

namespace {

std::shared_ptr<const qai_forge::resources::TopologyRegistry> requireTopology(
    std::shared_ptr<const qai_forge::resources::TopologyRegistry> topology) {
    if (!topology) {
        throw std::invalid_argument(
            "PlacementAdmissionGateway requires a topology registry");
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

std::shared_ptr<PlacementAdmissionGateway>
PlacementAdmissionGateway::createDefault() {
    auto topology = topologyFromEnvironment();
    return std::make_shared<PlacementAdmissionGateway>(
        std::move(topology),
        qai_forge::execution::ExecutionProviderRegistry::createDefault());
}

PlacementAdmissionGateway::PlacementAdmissionGateway(
    std::shared_ptr<const qai_forge::resources::TopologyRegistry> topology,
    qai_forge::execution::ExecutionProviderRegistry providers)
    : topology_(requireTopology(std::move(topology))),
      providers_(providers),
      resource_manager_(
          std::make_shared<qai_forge::resources::ResourceManager>(topology_)),
      resolver_(topology_, std::move(providers)) {}

std::shared_ptr<qai_forge::execution::IExecutionProvider>
PlacementAdmissionGateway::providerFor(const std::string& model_id) const {
    return providers_.get(providerForModel(model_id));
}

PlacementAdmissionResult PlacementAdmissionGateway::tryAcquire(
    const qai_forge::execution::RuntimeId& runtime_id,
    const std::string& model_id) const {
    const auto provider = providerFor(model_id);
    if (!provider) {
        PlacementAdmissionResult result;
        result.denial_reason =
            "No execution provider registered for model '" + model_id + "'";
        return result;
    }
    return tryAcquirePlan(
        runtime_id, model_id,
        provider->defaultPlan(model_id, *topology_));
}

PlacementAdmissionResult PlacementAdmissionGateway::tryAcquireAutomatic(
    const qai_forge::execution::RuntimeId& runtime_id,
    const std::string& model_id) const {
    PlacementAdmissionResult result;
    if (runtime_id.model_id.empty() || model_id.empty() ||
        runtime_id.model_id != model_id) {
        result.denial_reason =
            "Automatic placement requires matching non-empty runtime and model IDs";
        return result;
    }

    const auto provider = providerFor(model_id);
    if (!provider) {
        result.denial_reason =
            "No execution provider registered for model '" + model_id + "'";
        return result;
    }

    // Providers without physical-target enumeration (snpe/litert/litert_lm)
    // return their single defaultPlan() from candidatePlans() — handled
    // uniformly below rather than requiring a special case here.
    std::vector<qai_forge::execution::ExecutionPlan> candidates =
        provider->candidatePlans(model_id, *topology_);

    if (candidates.empty()) {
        result.denial_reason =
            "Automatic " + provider->id() +
            " placement produced no candidates";
        return result;
    }

    // Rank candidates by current shared load (fewest shared_users first),
    // then by resource id for determinism. Candidates with no physical
    // resource claim (automatic/backend-managed plans) sort last since
    // they carry no load signal to rank by — in practice this only
    // matters when candidates.size() > 1, which today only happens for
    // qnn/genie (physical-target enumeration).
    const auto snapshot = resource_manager_->snapshot();
    auto loadScore = [&](const qai_forge::execution::ExecutionPlan& plan) {
        uint32_t total = 0;
        bool have_resource = false;
        for (const auto& component : plan.components) {
            for (const auto& claim : component.claims) {
                have_resource = true;
                total += sharedUsersFor(snapshot, claim.resource);
            }
        }
        return std::make_pair(have_resource ? 0 : 1, total);
    };

    std::stable_sort(
        candidates.begin(), candidates.end(),
        [&](const qai_forge::execution::ExecutionPlan& lhs,
            const qai_forge::execution::ExecutionPlan& rhs) {
            return loadScore(lhs) < loadScore(rhs);
        });

    std::string last_denial;
    for (const auto& candidate : candidates) {
        PlacementAdmissionResult candidate_result =
            tryAcquirePlan(runtime_id, model_id, candidate);
        if (candidate_result.granted) {
            return candidate_result;
        }
        last_denial = candidate_result.denial_reason;
    }

    result.denial_reason =
        "Automatic " + provider->id() + " placement could not acquire a candidate";
    if (!last_denial.empty()) {
        result.denial_reason += ": " + last_denial;
    }
    return result;
}

PlacementAdmissionResult PlacementAdmissionGateway::tryAcquirePlan(
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

}  // namespace scheduler
