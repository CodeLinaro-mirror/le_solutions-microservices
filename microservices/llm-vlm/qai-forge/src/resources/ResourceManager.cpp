// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/resources/ResourceManager.h"

#include <algorithm>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace qai_forge {
namespace resources {

// -- ResourceLease ---------------------------------------------------------------

void ResourceLease::release() {
    if (state_ == nullptr) {
        return;
    }

    state_->releaseClaims(runtime_id_, granted_claims_);
    state_.reset();
    granted_claims_.clear();
}

// -- ResourceManagerState ------------------------------------------------------

void ResourceManagerState::releaseClaims(
    const execution::RuntimeId& /*runtime_id*/,
    const std::vector<execution::ResourceClaim>& claims) {
    std::lock_guard<std::mutex> lock(mutex);

    for (const auto& claim : claims) {
        auto it = capacity.find(claim.resource.value);
        if (it == capacity.end()) {
            continue;
        }

        Capacity& cap = it->second;
        uint32_t& used =
            claim.mode == execution::ResourceClaimMode::Shared
                ? cap.shared_used
                : cap.exclusive_used;

        if (used >= claim.capacity_units) {
            used -= claim.capacity_units;
        } else {
            used = 0;
        }
    }
}

// -- ResourceManager --------------------------------------------------------------

ResourceManager::ResourceManager(
    std::shared_ptr<const TopologyRegistry> topology)
    : topology_(std::move(topology)),
      state_(std::make_shared<ResourceManagerState>()) {
    if (!topology_) {
        return;
    }

    for (const auto& node : topology_->topology().nodes) {
        ResourceManagerState::Capacity cap;
        cap.total = node.capacity_units;
        state_->capacity[node.id.value] = cap;
    }
}

LeaseResult ResourceManager::tryAcquire(
    const execution::RuntimeId& runtime_id,
    const std::vector<execution::ResourceClaim>& claims) {
    LeaseResult result;

    if (state_ == nullptr) {
        result.denial_reason = "ResourceManager: state is unavailable";
        return result;
    }

    std::lock_guard<std::mutex> lock(state_->mutex);

    struct Demand {
        uint32_t shared = 0;
        uint32_t exclusive = 0;
    };
    std::unordered_map<std::string, Demand> required_demand;

    for (const auto& claim : claims) {
        if (claim.requirement != execution::ClaimRequirement::Required) {
            continue;
        }

        auto it = state_->capacity.find(claim.resource.value);
        if (it == state_->capacity.end()) {
            result.denial_reason =
                "Unknown resource: " + claim.resource.value;
            return result;
        }

        auto& demand = required_demand[claim.resource.value];
        uint32_t* requested =
            claim.mode == execution::ResourceClaimMode::Shared
                ? &demand.shared
                : &demand.exclusive;

        if (claim.capacity_units >
            std::numeric_limits<uint32_t>::max() - *requested) {
            result.denial_reason =
                "Required capacity overflow for resource: " +
                claim.resource.value;
            return result;
        }

        *requested += claim.capacity_units;
    }

    for (const auto& entry : required_demand) {
        const auto& resource_id = entry.first;
        const Demand& demand = entry.second;
        const ResourceManagerState::Capacity& cap =
            state_->capacity.at(resource_id);

        if (cap.unavailable) {
            result.denial_reason =
                "Resource unavailable: " + resource_id;
            return result;
        }

        const bool has_demand =
            demand.shared != 0 || demand.exclusive != 0;
        const bool conflicts_with_existing_exclusive =
            cap.exclusive_used != 0;
        const bool conflicts_between_requested_modes =
            demand.shared != 0 && demand.exclusive != 0;
        const uint64_t exclusive_requested =
            static_cast<uint64_t>(demand.exclusive);
        const uint64_t exclusive_used =
            static_cast<uint64_t>(cap.exclusive_used);

        if (!has_demand ||
            conflicts_with_existing_exclusive ||
            conflicts_between_requested_modes ||
            (demand.exclusive != 0 && cap.shared_used != 0) ||
            exclusive_requested + exclusive_used > cap.total) {
            std::ostringstream message;
            message << "Insufficient or conflicting capacity for '"
                    << resource_id
                    << "': requested_shared=" << demand.shared
                    << " requested_exclusive=" << demand.exclusive
                    << " available="
                    << (cap.total >= cap.used()
                            ? cap.total - cap.used()
                            : 0);
            result.denial_reason = message.str();
            return result;
        }
    }

    std::vector<execution::ResourceClaim> granted;
    granted.reserve(claims.size());

    for (const auto& claim : claims) {
        if (claim.requirement != execution::ClaimRequirement::Required) {
            continue;
        }

        auto& cap = state_->capacity.at(claim.resource.value);
        if (claim.mode == execution::ResourceClaimMode::Shared) {
            cap.shared_used += claim.capacity_units;
        } else {
            cap.exclusive_used += claim.capacity_units;
        }
        granted.push_back(claim);
    }

    for (const auto& claim : claims) {
        if (claim.requirement == execution::ClaimRequirement::Required) {
            continue;
        }

        auto it = state_->capacity.find(claim.resource.value);
        if (it == state_->capacity.end() || it->second.unavailable) {
            continue;
        }

        auto& cap = it->second;
        const bool can_share =
            claim.mode == execution::ResourceClaimMode::Shared &&
            cap.exclusive_used == 0;
        const bool can_exclude =
            claim.mode == execution::ResourceClaimMode::Exclusive &&
            cap.used() == 0;
        const uint64_t exclusive_total =
            static_cast<uint64_t>(cap.exclusive_used) +
            (claim.mode == execution::ResourceClaimMode::Exclusive
                ? claim.capacity_units
                : 0);

        if (exclusive_total <= cap.total && (can_share || can_exclude)) {
            if (claim.mode == execution::ResourceClaimMode::Shared) {
                cap.shared_used += claim.capacity_units;
            } else {
                cap.exclusive_used += claim.capacity_units;
            }
            granted.push_back(claim);
        }
    }

    result.granted = true;
    result.lease = ResourceLease(state_, runtime_id, std::move(granted));
    return result;
}

void ResourceManager::markUnavailable(
    const ResourceId& resource,
    FailureReason /*reason*/) {
    if (state_ == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(state_->mutex);
    auto it = state_->capacity.find(resource.value);
    if (it != state_->capacity.end()) {
        it->second.unavailable = true;
    }
}

void ResourceManager::clearUnavailable(const ResourceId& resource) {
    if (state_ == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(state_->mutex);
    auto it = state_->capacity.find(resource.value);
    if (it != state_->capacity.end()) {
        it->second.unavailable = false;
    }
}

ResourceSnapshot ResourceManager::snapshot() const {
    ResourceSnapshot snapshot;

    if (state_ == nullptr) {
        return snapshot;
    }

    std::lock_guard<std::mutex> lock(state_->mutex);
    snapshot.resources.reserve(state_->capacity.size());

    std::vector<std::string> resource_ids;
    resource_ids.reserve(state_->capacity.size());
    for (const auto& entry : state_->capacity) {
        resource_ids.push_back(entry.first);
    }
    std::sort(resource_ids.begin(), resource_ids.end());

    for (const auto& resource_id : resource_ids) {
        const auto& capacity = state_->capacity.at(resource_id);

        ResourceCapacitySnapshot entry;
        entry.resource = ResourceId(resource_id);
        entry.total_capacity = capacity.total;
        entry.used_capacity = capacity.used();
        entry.shared_users = capacity.shared_used;
        entry.unavailable = capacity.unavailable;
        snapshot.resources.push_back(std::move(entry));
    }

    return snapshot;
}

}  // namespace resources
}  // namespace qai_forge
