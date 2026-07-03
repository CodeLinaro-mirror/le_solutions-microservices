// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/scheduler/WarmModelPool.h"

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace scheduler {

enum class EvictionPolicyActionType {
    Activate,
    Drain,
    ProtectedDrain,
    Reject,
};

struct EvictionPolicyConfig {
    size_t max_active_models = 0;
    size_t max_concurrent_model_loads = 1;
    std::chrono::milliseconds idle_timeout{0};
    std::chrono::milliseconds model_residency_ttl{0};
    long memory_headroom_mb = 1024;
};

struct EvictionPolicyInput {
    ModelPoolSnapshot snapshot;
    EvictionPolicyConfig config;
    std::unordered_map<std::string, long> model_memory_mb;
    long available_memory_mb = 0;
    std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
};

struct EvictionPolicyAction {
    EvictionPolicyActionType type = EvictionPolicyActionType::Activate;
    std::string model_id;
};

struct EvictionPolicyPlan {
    std::vector<EvictionPolicyAction> actions;
    long available_memory_mb = 0;
    long memory_headroom_mb = 1024;
};

// Pure residency/admission planner.
//
// The pool owns ModelRuntime objects and executes these actions. This class only
// consumes snapshots and returns the next safe set of decisions.
class EvictionPolicy {
public:
    EvictionPolicyPlan plan(const EvictionPolicyInput& input) const;

private:
    struct WaitingCandidate {
        const ModelPoolRuntimeSnapshot* runtime = nullptr;
        long model_memory_mb = 0;
        long required_memory_mb = 0;
    };

    struct EvictionCandidate {
        const ModelPoolRuntimeSnapshot* runtime = nullptr;
        long model_memory_mb = 0;
    };

    static bool isActiveReservedState(ModelRuntimeState state);
    static bool isColdWaitingState(ModelRuntimeState state);
    static bool isProtectedDrainReady(const ModelPoolRuntimeSnapshot& runtime);
    static bool isIdleTtlExpired(const ModelPoolRuntimeSnapshot& runtime,
                                 const EvictionPolicyConfig& config,
                                 std::chrono::steady_clock::time_point now);
    static bool isEvictableIdleRuntime(const ModelPoolRuntimeSnapshot& runtime);
    static long modelMemoryMb(const EvictionPolicyInput& input,
                              const std::string& model_id);
    static bool waitingLess(const WaitingCandidate& lhs,
                            const WaitingCandidate& rhs);
    static bool victimLess(const EvictionCandidate& lhs,
                           const EvictionCandidate& rhs);
    static bool queueCandidateLess(const QueueAdmissionCandidate& lhs,
                                   const QueueAdmissionCandidate& rhs);
};

} // namespace scheduler
