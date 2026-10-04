// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/execution/ExecutionPlan.h"
#include "qai_forge/execution/ExecutionPlanResolver.h"
#include "qai_forge/execution/ExecutionProviderRegistry.h"
#include "qai_forge/resources/ResourceManager.h"
#include "qai_forge/resources/TopologyRegistry.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace scheduler {

// Move-only resource admission reservation owned by one resident runtime.
class PlacementAdmissionGateway;

class PlacementReservation {
public:
    PlacementReservation() = default;
    ~PlacementReservation() = default;

    PlacementReservation(const PlacementReservation&) = delete;
    PlacementReservation& operator=(const PlacementReservation&) = delete;
    PlacementReservation(PlacementReservation&&) noexcept = default;
    PlacementReservation& operator=(PlacementReservation&&) noexcept = default;

    explicit operator bool() const noexcept { return lease_.valid(); }
    bool valid() const noexcept { return lease_.valid(); }

    const qai_forge::execution::ExecutionPlan& plan() const noexcept {
        return plan_;
    }

    void release() noexcept { lease_.release(); }

private:
    friend class PlacementAdmissionGateway;

    PlacementReservation(
        qai_forge::execution::ExecutionPlan plan,
        qai_forge::resources::ResourceLease lease)
        : plan_(std::move(plan)), lease_(std::move(lease)) {}

    qai_forge::execution::ExecutionPlan plan_;
    qai_forge::resources::ResourceLease lease_;
};

struct PlacementAdmissionResult {
    bool granted = false;
    std::string denial_reason;
    PlacementReservation reservation;

    PlacementAdmissionResult() = default;
    PlacementAdmissionResult(PlacementAdmissionResult&&) noexcept = default;
    PlacementAdmissionResult& operator=(PlacementAdmissionResult&&) noexcept =
        default;
    PlacementAdmissionResult(const PlacementAdmissionResult&) = delete;
    PlacementAdmissionResult& operator=(const PlacementAdmissionResult&) =
        delete;
};

// Gatekeeps resource admission against the topology/provider registries.
// The granted reservation is transferred to one ModelRuntime or
// PredictiveModelRuntime, which passes the execution plan to its owned
// backend adapter.
//
// Per-provider plan construction (defaultPlan/candidatePlans) is delegated
// to the polymorphic qai_forge::execution::IExecutionProvider implementation
// resolved from providers_ — this class no longer embeds a branchy
// per-provider if/else chain (see qai_forge::execution::providers::*).
class PlacementAdmissionGateway {
public:
    static std::shared_ptr<PlacementAdmissionGateway> createDefault();

    PlacementAdmissionGateway(
        std::shared_ptr<const qai_forge::resources::TopologyRegistry> topology,
        qai_forge::execution::ExecutionProviderRegistry providers);

    PlacementAdmissionResult tryAcquire(
        const qai_forge::execution::RuntimeId& runtime_id,
        const std::string& model_id) const;

    // Selects and acquires a provider-compatible plan internally. Callers
    // do not provide a physical device ID.
    PlacementAdmissionResult tryAcquireAutomatic(
        const qai_forge::execution::RuntimeId& runtime_id,
        const std::string& model_id) const;

    const qai_forge::resources::ResourceManager& resourceManager() const noexcept {
        return *resource_manager_;
    }

    std::shared_ptr<const qai_forge::resources::TopologyRegistry> topology()
        const noexcept {
        return topology_;
    }

private:
    std::shared_ptr<qai_forge::execution::IExecutionProvider> providerFor(
        const std::string& model_id) const;

    PlacementAdmissionResult tryAcquirePlan(
        const qai_forge::execution::RuntimeId& runtime_id,
        const std::string& model_id,
        const qai_forge::execution::ExecutionPlan& plan) const;

    std::shared_ptr<const qai_forge::resources::TopologyRegistry> topology_;
    qai_forge::execution::ExecutionProviderRegistry providers_;
    std::shared_ptr<qai_forge::resources::ResourceManager> resource_manager_;
    qai_forge::execution::ExecutionPlanResolver resolver_;
};

}  // namespace scheduler
