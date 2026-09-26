// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// -----------------------------------------------------------------------------
// ResourceManager - Static resource admission and RAII leases
//
// ResourceManager tracks capacity for each ResourceNode in a TopologyRegistry
// and grants/releases ResourceLease objects representing a runtime's claim on
// one or more resources. It composes with (does not replace) the existing
// ModelLoadCoordinator, which continues to own process/system-memory
// admission.
//
// HTP/runtime claims default to Shared because Hexagon can multiplex
// multiple runtime contexts. Explicit Exclusive claims reserve a resource for
// one runtime and conflict with both shared and other exclusive claims.
// -----------------------------------------------------------------------------

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "qai_forge/execution/ExecutionPlan.h"
#include "qai_forge/resources/ResourceTypes.h"
#include "qai_forge/resources/TopologyRegistry.h"

namespace qai_forge {
namespace resources {

struct ResourceLeaseState {
    virtual ~ResourceLeaseState() = default;

    virtual void releaseClaims(
        const execution::RuntimeId& runtime_id,
        const std::vector<execution::ResourceClaim>& claims) = 0;
};

struct ResourceManagerState final : ResourceLeaseState {
    struct Capacity {
        uint32_t total = 0;
        uint32_t shared_used = 0;
        uint32_t exclusive_used = 0;
        bool unavailable = false;

        uint32_t used() const {
            return exclusive_used != 0 ? exclusive_used
                                       : (shared_used != 0 ? 1 : 0);
        }
    };

    mutable std::mutex mutex;
    std::unordered_map<std::string, Capacity> capacity;

    void releaseClaims(
        const execution::RuntimeId& runtime_id,
        const std::vector<execution::ResourceClaim>& claims) override;
};

// -- ResourceLease --------------------------------------------------------------
// Move-only RAII handle representing a granted set of resource claims for one
// RuntimeId.
class ResourceManager;

class ResourceLease {
public:
    ResourceLease() = default;
    ResourceLease(const ResourceLease&) = delete;
    ResourceLease& operator=(const ResourceLease&) = delete;

    ResourceLease(ResourceLease&& other) noexcept { moveFrom(other); }
    ResourceLease& operator=(ResourceLease&& other) noexcept {
        if (this != &other) {
            release();
            moveFrom(other);
        }
        return *this;
    }

    ~ResourceLease() { release(); }

    bool valid() const { return state_ != nullptr; }
    const execution::RuntimeId& runtimeId() const { return runtime_id_; }
    const std::vector<execution::ResourceClaim>& grantedClaims() const {
        return granted_claims_;
    }

    void release();

private:
    friend class ResourceManager;

    ResourceLease(std::shared_ptr<ResourceLeaseState> state,
                  execution::RuntimeId runtime_id,
                  std::vector<execution::ResourceClaim> granted_claims)
        : state_(std::move(state)),
          runtime_id_(std::move(runtime_id)),
          granted_claims_(std::move(granted_claims)) {}

    void moveFrom(ResourceLease& other) {
        state_ = std::move(other.state_);
        runtime_id_ = std::move(other.runtime_id_);
        granted_claims_ = std::move(other.granted_claims_);
        other.granted_claims_.clear();
    }

    std::shared_ptr<ResourceLeaseState> state_;
    execution::RuntimeId runtime_id_;
    std::vector<execution::ResourceClaim> granted_claims_;
};

// -- LeaseResult ------------------------------------------------------------------
struct LeaseResult {
    bool granted = false;
    std::string denial_reason;   // populated iff !granted
    ResourceLease lease;         // valid iff granted

    LeaseResult() = default;
    LeaseResult(LeaseResult&&) = default;
    LeaseResult& operator=(LeaseResult&&) = default;
    LeaseResult(const LeaseResult&) = delete;
    LeaseResult& operator=(const LeaseResult&) = delete;
};

// -- ResourceSnapshot --------------------------------------------------------------
struct ResourceCapacitySnapshot {
    ResourceId resource;
    uint32_t total_capacity = 0;
    uint32_t used_capacity = 0;
    uint32_t shared_users = 0;
    bool unavailable = false;
};

struct ResourceSnapshot {
    std::vector<ResourceCapacitySnapshot> resources;
};

enum class FailureReason {
    HardwareFault,
    WorkerCrashLoop,
    ManualQuarantine
};

// -- ResourceManager --------------------------------------------------------------
class ResourceManager {
public:
    explicit ResourceManager(std::shared_ptr<const TopologyRegistry> topology);

    // Attempts to grant all Required claims and as many Possible claims as
    // admission allows. HTP claims default to Shared; providers that require
    // sole ownership must explicitly request Exclusive.
    LeaseResult tryAcquire(const execution::RuntimeId& runtime_id,
                            const std::vector<execution::ResourceClaim>& claims);

    void markUnavailable(const ResourceId& resource, FailureReason reason);
    void clearUnavailable(const ResourceId& resource);

    ResourceSnapshot snapshot() const;

private:
    friend class ResourceLease;

    std::shared_ptr<const TopologyRegistry> topology_;
    std::shared_ptr<ResourceManagerState> state_;
};

}  // namespace resources
}  // namespace qai_forge
