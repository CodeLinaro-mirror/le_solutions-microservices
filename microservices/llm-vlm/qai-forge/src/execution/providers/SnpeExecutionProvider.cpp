// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/execution/providers/SnpeExecutionProvider.h"

#include "qai_forge/execution/SnpeLaunchOptions.h"

#include <cstdlib>

namespace qai_forge {
namespace execution {

namespace {

std::string snpeRuntimeOrderFromEnvironment() {
    const char* delegate = std::getenv("SNPE_DELEGATE");
    const std::string requested = delegate ? delegate : "dsp";

    if (requested == "dsp") return "DSP,CPU";
    if (requested == "aip") return "AIP_FIXED8_TF,CPU";
    if (requested == "gpu") return "GPU,CPU";
    if (requested == "none" || requested == "cpu") return "CPU";
    return "DSP,CPU";
}

}  // namespace

ProviderCapabilities SnpeExecutionProvider::capabilities() const {
    ProviderCapabilities caps;
    caps.enumerates_physical_targets = false;
    caps.supports_exact_binding = false;
    caps.supports_runtime_preference = true;
    caps.supports_fallback = true;
    caps.supports_multi_target = false;
    caps.reports_effective_target = false;
    caps.requires_process_isolation = true;
    return caps;
}

ExecutionPlan SnpeExecutionProvider::defaultPlan(
    const std::string& /*model_id*/,
    const resources::TopologyRegistry& topology) const {
    ExecutionPlan plan;
    plan.provider = "snpe";

    ComponentPlacement component;
    component.role = ComponentRole::MainGraph;
    component.target.provider = "snpe";
    component.target.target_id =
        "runtime_order=" + snpeRuntimeOrderFromEnvironment();
    component.target.semantics = TargetSemantics::RuntimePreference;
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

nlohmann::ordered_json SnpeExecutionProvider::toInitOptions(
    const ExecutionPlan& plan) const {
    return SnpeLaunchOptions::toInitOptions(plan);
}

}  // namespace execution
}  // namespace qai_forge
