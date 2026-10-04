// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// IExecutionProvider - one polymorphic implementation per execution
// provider ("qnn", "genie", "llamacpp", "snpe", "litert", "litert_lm").
//
// Replaces the per-provider if/else branches previously inlined in
// PlacementAdmissionGateway::defaultPlan()/tryAcquireAutomatic(). Adding a
// new provider means implementing one subclass and registering it in
// ExecutionProviderRegistry::createDefault() - no shared function changes.
//
// toInitOptions() forwards to the existing *LaunchOptions static classes
// (QnnLaunchOptions, SnpeLaunchOptions, etc.) - that translation logic is
// unchanged, only relocated behind a common interface.

#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "qai_forge/execution/ExecutionPlan.h"
#include "qai_forge/resources/TopologyRegistry.h"

namespace qai_forge {
namespace execution {

class IExecutionProvider {
public:
    virtual ~IExecutionProvider() = default;

    // Must match the key this provider is registered under.
    virtual std::string id() const = 0;

    virtual ProviderCapabilities capabilities() const = 0;

    // The provider's single automatic/backend-managed plan for model_id.
    virtual ExecutionPlan defaultPlan(
        const std::string& model_id,
        const resources::TopologyRegistry& topology) const = 0;

    // Every placement candidate this provider can propose for model_id
    // (e.g. one exact plan per physical HTP it can address by native ID).
    // Caller filters by resource health/load and acquires a lease; this
    // performs no resource-manager I/O.
    //
    // Default: single-element vector containing defaultPlan() - sufficient
    // for providers with no physical-target enumeration (snpe, litert,
    // litert_lm). Providers that can enumerate physical targets (qnn,
    // genie) override this.
    virtual std::vector<ExecutionPlan> candidatePlans(
        const std::string& model_id,
        const resources::TopologyRegistry& topology) const {
        return {defaultPlan(model_id, topology)};
    }

    // Translates a resolved ExecutionPlan into provider-native worker INIT
    // options JSON.
    virtual nlohmann::ordered_json toInitOptions(
        const ExecutionPlan& plan) const = 0;
};

}  // namespace execution
}  // namespace qai_forge
