// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "scheduler/EvictionPolicy.h"

#include <algorithm>
#include <unordered_set>

namespace scheduler {

EvictionPolicyPlan EvictionPolicy::plan(
    const EvictionPolicyInput& input) const {
    EvictionPolicyPlan plan;
    plan.available_memory_mb = input.available_memory_mb;
    plan.memory_headroom_mb = input.config.memory_headroom_mb;

    std::unordered_set<std::string> planned_model_ids;

    auto add_action =
        [&plan, &planned_model_ids](EvictionPolicyActionType type,
                                    const std::string& model_id) {
            if (model_id.empty() ||
                planned_model_ids.count(model_id) > 0) {
                return;
            }
            plan.actions.push_back(EvictionPolicyAction{type, model_id});
            planned_model_ids.insert(model_id);
        };

    for (const auto& runtime : input.snapshot.runtimes) {
        if (isProtectedDrainReady(runtime)) {
            add_action(EvictionPolicyActionType::ProtectedDrain, runtime.model_id);
        }
    }

    for (const auto& runtime : input.snapshot.runtimes) {
        if (planned_model_ids.count(runtime.model_id) > 0) {
            continue;
        }
        if (isIdleTtlExpired(runtime, input.config, input.now)) {
            add_action(EvictionPolicyActionType::Drain, runtime.model_id);
        }
    }

    std::vector<WaitingCandidate> waiting;
    std::vector<EvictionCandidate> evictable;
    long eventual_reclaimable_mb = 0;
    size_t loading_count = 0;

    for (const auto& runtime : input.snapshot.runtimes) {
        const long model_memory_mb = modelMemoryMb(input, runtime.model_id);

        if (runtime.active_reserved && isActiveReservedState(runtime.state)) {
            eventual_reclaimable_mb += model_memory_mb;
        }

        if (runtime.active_reserved &&
            runtime.state == ModelRuntimeState::Loading) {
            ++loading_count;
        }

        if (isColdWaitingState(runtime.state) &&
            !runtime.active_reserved &&
            runtime.candidate.has_work) {
            waiting.push_back(WaitingCandidate{
                &runtime,
                model_memory_mb,
                model_memory_mb + input.config.memory_headroom_mb,
            });
        }

        if (planned_model_ids.count(runtime.model_id) == 0 &&
            isEvictableIdleRuntime(runtime)) {
            evictable.push_back(EvictionCandidate{&runtime, model_memory_mb});
        }
    }

    std::sort(waiting.begin(), waiting.end(), waitingLess);
    std::sort(evictable.begin(), evictable.end(), victimLess);

    if (input.config.max_active_models == 0) {
        for (const WaitingCandidate& candidate : waiting) {
            add_action(EvictionPolicyActionType::Reject,
                       candidate.runtime->model_id);
        }
        return plan;
    }

    long remaining_available_mb = input.available_memory_mb;
    size_t active_reserved_count = input.snapshot.active_reserved_models;
    const size_t max_concurrent_model_loads =
        input.config.max_concurrent_model_loads > 0
            ? input.config.max_concurrent_model_loads
            : 1;
    size_t available_load_permits =
        loading_count < max_concurrent_model_loads
            ? max_concurrent_model_loads - loading_count
            : 0;

    for (const WaitingCandidate& candidate : waiting) {
        if (!candidate.runtime) {
            continue;
        }

        if (input.available_memory_mb + eventual_reclaimable_mb <
            candidate.required_memory_mb) {
            add_action(EvictionPolicyActionType::Reject,
                       candidate.runtime->model_id);
            continue;
        }

        if (available_load_permits > 0 &&
            active_reserved_count + 1 <= input.config.max_active_models &&
            remaining_available_mb >= candidate.required_memory_mb) {
            add_action(EvictionPolicyActionType::Activate,
                       candidate.runtime->model_id);
            remaining_available_mb -= candidate.required_memory_mb;
            ++active_reserved_count;
            --available_load_permits;
            continue;
        }

        const size_t count_evictions_needed =
            active_reserved_count + 1 > input.config.max_active_models
                ? active_reserved_count + 1 - input.config.max_active_models
                : 0;
        const long memory_needed_mb =
            remaining_available_mb < candidate.required_memory_mb
                ? candidate.required_memory_mb - remaining_available_mb
                : 0;

        size_t planned_eviction_count = 0;
        long planned_freed_mb = 0;
        std::vector<std::string> victims_for_candidate;

        for (const EvictionCandidate& victim : evictable) {
            if (!victim.runtime ||
                planned_model_ids.count(victim.runtime->model_id) > 0) {
                continue;
            }

            victims_for_candidate.push_back(victim.runtime->model_id);
            planned_freed_mb += victim.model_memory_mb;
            ++planned_eviction_count;

            if (planned_eviction_count >= count_evictions_needed &&
                planned_freed_mb >= memory_needed_mb) {
                break;
            }
        }

        if (planned_eviction_count >= count_evictions_needed &&
            planned_freed_mb >= memory_needed_mb) {
            for (const std::string& victim_model_id : victims_for_candidate) {
                add_action(EvictionPolicyActionType::Drain, victim_model_id);
            }
        }

        // Eviction is asynchronous. Once victims report NotResident, the pool
        // wakes and re-runs this policy over every waiting runtime in priority
        // order. Do not let lower-priority cold models bypass this candidate.
        break;
    }

    return plan;
}

bool EvictionPolicy::isActiveReservedState(ModelRuntimeState state) {
    return state == ModelRuntimeState::Loading ||
           state == ModelRuntimeState::Idle ||
           state == ModelRuntimeState::Running ||
           state == ModelRuntimeState::Draining ||
           state == ModelRuntimeState::Evicting;
}

bool EvictionPolicy::isColdWaitingState(ModelRuntimeState state) {
    return state == ModelRuntimeState::NotResident ||
           state == ModelRuntimeState::Failed;
}

bool EvictionPolicy::isProtectedDrainReady(
    const ModelPoolRuntimeSnapshot& runtime) {
    return runtime.expiry_pending &&
           runtime.active_reserved &&
           !runtime.eviction_requested &&
           !runtime.tool_lease_active &&
           (runtime.state == ModelRuntimeState::Idle ||
            runtime.state == ModelRuntimeState::Draining);
}

bool EvictionPolicy::isIdleTtlExpired(
    const ModelPoolRuntimeSnapshot& runtime,
    const EvictionPolicyConfig& config,
    std::chrono::steady_clock::time_point now) {
    return config.idle_timeout.count() > 0 &&
           runtime.state == ModelRuntimeState::Idle &&
           runtime.active_reserved &&
           !runtime.eviction_requested &&
           !runtime.tool_lease_active &&
           !runtime.expiry_pending &&
           runtime.queue.total() == 0 &&
           runtime.idle_since.has_value() &&
           now - runtime.idle_since.value() >= config.idle_timeout;
}

bool EvictionPolicy::isEvictableIdleRuntime(
    const ModelPoolRuntimeSnapshot& runtime) {
    return runtime.state == ModelRuntimeState::Idle &&
           runtime.active_reserved &&
           !runtime.eviction_requested &&
           !runtime.tool_lease_active &&
           runtime.queue.total() == 0;
}

long EvictionPolicy::modelMemoryMb(
    const EvictionPolicyInput& input,
    const std::string& model_id) {
    auto it = input.model_memory_mb.find(model_id);
    if (it == input.model_memory_mb.end() || it->second <= 0) {
        return 4096;
    }
    return it->second;
}

bool EvictionPolicy::waitingLess(
    const WaitingCandidate& lhs,
    const WaitingCandidate& rhs) {
    if (!lhs.runtime) {
        return false;
    }
    if (!rhs.runtime) {
        return true;
    }

    if (queueCandidateLess(lhs.runtime->candidate, rhs.runtime->candidate)) {
        return true;
    }
    if (queueCandidateLess(rhs.runtime->candidate, lhs.runtime->candidate)) {
        return false;
    }
    return lhs.runtime->model_id < rhs.runtime->model_id;
}

bool EvictionPolicy::victimLess(
    const EvictionCandidate& lhs,
    const EvictionCandidate& rhs) {
    if (!lhs.runtime) {
        return false;
    }
    if (!rhs.runtime) {
        return true;
    }

    if (lhs.runtime->last_used_at != rhs.runtime->last_used_at) {
        return lhs.runtime->last_used_at < rhs.runtime->last_used_at;
    }
    return lhs.runtime->model_id < rhs.runtime->model_id;
}

bool EvictionPolicy::queueCandidateLess(
    const QueueAdmissionCandidate& lhs,
    const QueueAdmissionCandidate& rhs) {
    if (!lhs.has_work) {
        return false;
    }
    if (!rhs.has_work) {
        return true;
    }

    if (lhs.priority != rhs.priority) {
        return static_cast<int>(lhs.priority) < static_cast<int>(rhs.priority);
    }

    if (lhs.created_at != rhs.created_at) {
        return lhs.created_at < rhs.created_at;
    }

    return lhs.job_id < rhs.job_id;
}

} // namespace scheduler
