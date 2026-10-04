// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/execution/providers/LiteRtLmExecutionProvider.h"

#include "qai_forge/execution/LiteRtLmLaunchOptions.h"

#include <cstdlib>

namespace qai_forge {
namespace execution {

ProviderCapabilities LiteRtLmExecutionProvider::capabilities() const {
    ProviderCapabilities caps;
    caps.enumerates_physical_targets = false;
    caps.supports_exact_binding = false;
    caps.supports_runtime_preference = false;
    caps.supports_fallback = true;
    caps.supports_multi_target = false;
    caps.reports_effective_target = false;
    caps.requires_process_isolation = true;
    return caps;
}

ExecutionPlan LiteRtLmExecutionProvider::defaultPlan(
    const std::string& /*model_id*/,
    const resources::TopologyRegistry& topology) const {
    ExecutionPlan plan;
    plan.provider = "litert_lm";

    ComponentPlacement component;
    component.role = ComponentRole::MainGraph;
    component.target.provider = "litert_lm";

    const char* env = std::getenv("LITERT_LM_BACKEND");
    const std::string backend = (env && env[0] != '\0') ? env : "npu";
    component.target.target_id = "backend=" + backend;
    component.target.semantics = TargetSemantics::BackendAutomatic;
    component.target.discovery_mode = DiscoveryMode::Configured;
    component.target.binding_strength = BindingStrength::Preferred;

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

nlohmann::ordered_json LiteRtLmExecutionProvider::toInitOptions(
    const ExecutionPlan& plan) const {
    return LiteRtLmLaunchOptions::toInitOptions(plan);
}

}  // namespace execution
}  // namespace qai_forge
