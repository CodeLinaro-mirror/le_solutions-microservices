// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/execution/ExecutionPlanResolver.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>

namespace qai_forge {
namespace execution {

namespace {

bool supportsTargetSemantics(
    const BackendTarget& target,
    const ProviderCapabilities& capabilities) {
    switch (target.semantics) {
        case TargetSemantics::PhysicalInstance:
        case TargetSemantics::LogicalEndpoint:
            return capabilities.supports_exact_binding ||
                   (target.discovery_mode == DiscoveryMode::Configured &&
                    target.binding_strength == BindingStrength::Preferred &&
                    capabilities.supports_multi_target);

        case TargetSemantics::RuntimePreference:
            return capabilities.supports_runtime_preference;

        case TargetSemantics::AcceleratorClassSet:
            return capabilities.supports_runtime_preference ||
                   capabilities.supports_fallback;

        case TargetSemantics::BackendAutomatic:
            return true;
    }

    return false;
}

bool claimLess(const ResourceClaim& lhs, const ResourceClaim& rhs) {
    if (lhs.resource.value != rhs.resource.value) {
        return lhs.resource.value < rhs.resource.value;
    }
    if (lhs.requirement != rhs.requirement) {
        return static_cast<int>(lhs.requirement) <
               static_cast<int>(rhs.requirement);
    }
    if (lhs.mode != rhs.mode) {
        return static_cast<int>(lhs.mode) < static_cast<int>(rhs.mode);
    }
    return lhs.capacity_units < rhs.capacity_units;
}

std::string normalizedNativeTarget(
    const std::string& provider,
    const std::string& target_id) {
    if ((provider == "qnn" || provider == "genie") &&
        target_id.rfind("device_id=", 0) == 0) {
        return target_id.substr(std::string("device_id=").size());
    }
    return target_id;
}

const resources::ResourceNode* findNativeTargetResource(
    const resources::TopologyRegistry& topology,
    const std::string& provider,
    TargetSemantics semantics,
    const std::string& target_id) {
    if (semantics != TargetSemantics::PhysicalInstance &&
        semantics != TargetSemantics::LogicalEndpoint) {
        return nullptr;
    }

    const std::string native_target =
        normalizedNativeTarget(provider, target_id);
    for (const auto* node :
         topology.byKind(resources::ResourceKind::Htp)) {
        auto it = node->native_ids.find(provider);
        if (it != node->native_ids.end() &&
            it->second == native_target) {
            return node;
        }
    }
    return nullptr;
}

bool componentLess(
    const ComponentPlacement& lhs,
    const ComponentPlacement& rhs) {
    if (lhs.role != rhs.role) {
        return static_cast<int>(lhs.role) < static_cast<int>(rhs.role);
    }
    if (lhs.target.provider != rhs.target.provider) {
        return lhs.target.provider < rhs.target.provider;
    }
    return lhs.target.target_id < rhs.target.target_id;
}

}  // namespace

ExecutionPlanResolver::ExecutionPlanResolver(
    std::shared_ptr<const resources::TopologyRegistry> topology,
    ProviderRegistry providers)
    : topology_(std::move(topology)),
      providers_(std::move(providers)) {}

bool ExecutionPlanResolver::resourceExists(
    const resources::ResourceId& resource) const {
    return topology_ != nullptr && topology_->find(resource) != nullptr;
}

PlanValidationResult ExecutionPlanResolver::validateClaim(
    const ResourceClaim& claim) const {
    if (claim.resource.value.empty()) {
        return PlanValidationResult::failure(
            "ExecutionPlanResolver: claim has an empty resource ID");
    }
    if (claim.capacity_units == 0) {
        return PlanValidationResult::failure(
            "ExecutionPlanResolver: claim for '" + claim.resource.value +
            "' has zero capacity");
    }
    if (!resourceExists(claim.resource)) {
        return PlanValidationResult::failure(
            "ExecutionPlanResolver: claim references unknown resource '" +
            claim.resource.value + "'");
    }
    return PlanValidationResult::success(ExecutionPlan{});
}

PlanValidationResult ExecutionPlanResolver::validateTarget(
    const ExecutionPlan& plan,
    const ComponentPlacement& component,
    const ProviderCapabilities& capabilities) const {
    if (component.target.provider.empty()) {
        return PlanValidationResult::failure(
            "ExecutionPlanResolver: component target has an empty provider");
    }
    if (component.target.provider != plan.provider) {
        return PlanValidationResult::failure(
            "ExecutionPlanResolver: component target provider '" +
            component.target.provider + "' does not match plan provider '" +
            plan.provider + "'");
    }
    if (component.target.semantics != TargetSemantics::BackendAutomatic &&
        component.target.target_id.empty()) {
        return PlanValidationResult::failure(
            "ExecutionPlanResolver: non-automatic target has an empty target ID");
    }
    if (component.target.binding_strength == BindingStrength::Exact &&
        !capabilities.supports_exact_binding) {
        return PlanValidationResult::failure(
            "ExecutionPlanResolver: provider '" + plan.provider +
            "' cannot guarantee exact binding");
    }
    if (!supportsTargetSemantics(component.target, capabilities)) {
        std::ostringstream message;
        message << "ExecutionPlanResolver: provider '" << plan.provider
                << "' does not support target semantics '"
                << toString(component.target.semantics) << "'";
        return PlanValidationResult::failure(message.str());
    }
    if (component.target.binding_strength == BindingStrength::Preferred &&
        component.target.semantics == TargetSemantics::RuntimePreference &&
        !capabilities.supports_runtime_preference) {
        return PlanValidationResult::failure(
            "ExecutionPlanResolver: provider '" + plan.provider +
            "' does not support runtime preference");
    }

    if (component.target.semantics == TargetSemantics::PhysicalInstance ||
        component.target.semantics == TargetSemantics::LogicalEndpoint) {
        const auto* target_resource = findNativeTargetResource(
            *topology_, plan.provider, component.target.semantics,
            component.target.target_id);
        if (target_resource == nullptr) {
            return PlanValidationResult::failure(
                "ExecutionPlanResolver: target '" +
                component.target.target_id +
                "' does not map to a physical HTP for provider '" +
                plan.provider + "'");
        }

        for (const auto& claim : component.claims) {
            const auto* claimed_resource = topology_->find(claim.resource);
            if (claimed_resource != nullptr &&
                claimed_resource->kind == resources::ResourceKind::Htp &&
                claimed_resource->id != target_resource->id) {
                return PlanValidationResult::failure(
                    "ExecutionPlanResolver: target '" +
                    component.target.target_id + "' maps to '" +
                    target_resource->id.value +
                    "' but component claims '" +
                    claimed_resource->id.value + "'");
            }
        }
    }

    return PlanValidationResult::success(ExecutionPlan{});
}

PlanValidationResult ExecutionPlanResolver::resolve(
    const ExecutionPlan& plan) const {
    if (plan.provider.empty()) {
        return PlanValidationResult::failure(
            "ExecutionPlanResolver: plan has an empty provider");
    }
    if (!topology_) {
        return PlanValidationResult::failure(
            "ExecutionPlanResolver: topology registry is null");
    }
    if (!providers_.isRegistered(plan.provider)) {
        return PlanValidationResult::failure(
            "ExecutionPlanResolver: unknown provider '" + plan.provider + "'");
    }
    if (plan.components.empty()) {
        return PlanValidationResult::failure(
            "ExecutionPlanResolver: plan has no components");
    }

    const auto capabilities = providers_.capabilitiesOf(plan.provider);
    if (!capabilities.has_value()) {
        return PlanValidationResult::failure(
            "ExecutionPlanResolver: capabilities missing for provider '" +
            plan.provider + "'");
    }

    ExecutionPlan normalized = plan;
    for (const auto& component : normalized.components) {
        auto target_result =
            validateTarget(normalized, component, *capabilities);
        if (!target_result.valid) {
            return target_result;
        }

        for (const auto& claim : component.claims) {
            auto claim_result = validateClaim(claim);
            if (!claim_result.valid) {
                return claim_result;
            }
        }
    }

    std::sort(
        normalized.components.begin(),
        normalized.components.end(),
        componentLess);
    for (auto& component : normalized.components) {
        std::sort(component.claims.begin(), component.claims.end(), claimLess);
    }

    return PlanValidationResult::success(std::move(normalized));
}

}  // namespace execution
}  // namespace qai_forge
