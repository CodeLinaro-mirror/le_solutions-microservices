// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/execution/providers/QnnExecutionProvider.h"

#include "qai_forge/execution/QnnLaunchOptions.h"

#include <algorithm>
#include <limits>

namespace qai_forge {
namespace execution {

ProviderCapabilities QnnExecutionProvider::capabilities() const {
    ProviderCapabilities caps;
    caps.enumerates_physical_targets = true;
    caps.supports_exact_binding = true;
    caps.supports_runtime_preference = false;
    caps.supports_fallback = false;
    caps.supports_multi_target = false;
    caps.reports_effective_target = true;
    caps.requires_process_isolation = true;
    return caps;
}

ExecutionPlan QnnExecutionProvider::defaultPlan(
    const std::string& /*model_id*/,
    const resources::TopologyRegistry& topology) const {
    ExecutionPlan plan;
    plan.provider = "qnn";

    ComponentPlacement component;
    component.role = ComponentRole::MainGraph;
    component.target.provider = "qnn";
    component.target.semantics = TargetSemantics::BackendAutomatic;
    component.target.discovery_mode = DiscoveryMode::BackendManaged;
    component.target.binding_strength = BindingStrength::Automatic;

    if (topology.find(resources::ResourceId("cdsp:0")) != nullptr) {
        component.claims.push_back({
            resources::ResourceId("cdsp:0"),
            1,
            ClaimRequirement::Required,
            ResourceClaimMode::Shared});
    }

    plan.components.push_back(std::move(component));
    return plan;
}

ExecutionPlan QnnExecutionProvider::exactPlan(
    uint32_t qnn_device_id,
    const resources::TopologyRegistry& topology) {
    ExecutionPlan plan;
    plan.provider = "qnn";

    ComponentPlacement component;
    component.role = ComponentRole::MainGraph;
    component.target.provider = "qnn";
    component.target.target_id =
        "device_id=" + std::to_string(qnn_device_id);
    component.target.semantics = TargetSemantics::PhysicalInstance;
    component.target.discovery_mode = DiscoveryMode::Enumerated;
    component.target.binding_strength = BindingStrength::Exact;

    const resources::ResourceNode* selected_node = nullptr;
    for (const auto* node :
         topology.byKind(resources::ResourceKind::Htp)) {
        if (!node) continue;
        const auto native_id = node->native_ids.find("qnn");
        if (native_id != node->native_ids.end() &&
            native_id->second == std::to_string(qnn_device_id)) {
            selected_node = node;
            break;
        }
    }

    const resources::ResourceId selected_resource =
        selected_node
            ? selected_node->id
            : resources::ResourceId("htp:" + std::to_string(qnn_device_id));

    component.claims.push_back({
        selected_resource,
        1,
        ClaimRequirement::Required,
        ResourceClaimMode::Shared});

    if (selected_node && selected_node->parent.has_value()) {
        component.claims.push_back({
            selected_node->parent.value(),
            1,
            ClaimRequirement::Required,
            ResourceClaimMode::Shared});
    }

    plan.components.push_back(std::move(component));
    return plan;
}

std::vector<ExecutionPlan> QnnExecutionProvider::candidatePlans(
    const std::string& /*model_id*/,
    const resources::TopologyRegistry& topology) const {
    std::vector<ExecutionPlan> candidates;

    for (const auto* node :
         topology.byKind(resources::ResourceKind::Htp)) {
        if (!node) continue;

        const auto native_id = node->native_ids.find("qnn");
        if (native_id == node->native_ids.end()) continue;

        try {
            size_t consumed = 0;
            const unsigned long long parsed =
                std::stoull(native_id->second, &consumed, 10);
            if (consumed != native_id->second.size() ||
                parsed > std::numeric_limits<uint32_t>::max()) {
                continue;
            }
            candidates.push_back(
                exactPlan(static_cast<uint32_t>(parsed), topology));
        } catch (...) {
            continue;
        }
    }

    if (candidates.empty()) {
        candidates.push_back(defaultPlan("", topology));
    }
    return candidates;
}

nlohmann::ordered_json QnnExecutionProvider::toInitOptions(
    const ExecutionPlan& plan) const {
    return QnnLaunchOptions::toInitOptions(plan);
}

}  // namespace execution
}  // namespace qai_forge
