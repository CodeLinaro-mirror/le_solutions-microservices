// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ExecutionPlanResolver - deterministic plan validation.
//
// Validates provider-neutral execution plans before resource admission or
// worker launch. Performs no hardware I/O, no scheduler mutation, and no
// provider-native translation.

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "qai_forge/execution/ExecutionPlan.h"
#include "qai_forge/execution/ProviderRegistry.h"
#include "qai_forge/resources/TopologyRegistry.h"

namespace qai_forge {
namespace execution {

struct PlanValidationResult {
    bool valid = false;
    std::string error;
    ExecutionPlan normalized_plan;

    static PlanValidationResult success(ExecutionPlan plan) {
        PlanValidationResult result;
        result.valid = true;
        result.normalized_plan = std::move(plan);
        return result;
    }

    static PlanValidationResult failure(std::string message) {
        PlanValidationResult result;
        result.valid = false;
        result.error = std::move(message);
        return result;
    }
};

class ExecutionPlanResolver {
public:
    ExecutionPlanResolver(
        std::shared_ptr<const resources::TopologyRegistry> topology,
        ProviderRegistry providers);

    // Validates and normalizes one plan. The input plan is never modified.
    PlanValidationResult resolve(const ExecutionPlan& plan) const;

private:
    bool resourceExists(const resources::ResourceId& resource) const;
    PlanValidationResult validateTarget(
        const ExecutionPlan& plan,
        const ComponentPlacement& component,
        const ProviderCapabilities& capabilities) const;
    PlanValidationResult validateClaim(
        const ResourceClaim& claim) const;

    std::shared_ptr<const resources::TopologyRegistry> topology_;
    ProviderRegistry providers_;
};

}  // namespace execution
}  // namespace qai_forge
