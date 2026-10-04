// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/execution/providers/LlamaCppExecutionProvider.h"

#include "qai_forge/execution/LlamaCppLaunchOptions.h"

namespace qai_forge {
namespace execution {

ProviderCapabilities LlamaCppExecutionProvider::capabilities() const {
    ProviderCapabilities caps;
    caps.enumerates_physical_targets = false;
    caps.supports_exact_binding = true;
    caps.supports_runtime_preference = false;
    caps.supports_fallback = false;
    caps.supports_multi_target = true;
    caps.reports_effective_target = true;
    caps.requires_process_isolation = true;
    return caps;
}

ExecutionPlan LlamaCppExecutionProvider::defaultPlan(
    const std::string& /*model_id*/,
    const resources::TopologyRegistry& topology) const {
    ExecutionPlan plan;
    plan.provider = "llamacpp";

    ComponentPlacement component;
    component.role = ComponentRole::MainGraph;
    component.target.provider = "llamacpp";
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

nlohmann::ordered_json LlamaCppExecutionProvider::toInitOptions(
    const ExecutionPlan& plan) const {
    return LlamaCppLaunchOptions::toInitOptions(plan);
}

}  // namespace execution
}  // namespace qai_forge
