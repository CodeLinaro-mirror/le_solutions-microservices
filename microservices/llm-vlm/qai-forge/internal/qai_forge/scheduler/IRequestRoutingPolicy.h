// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// IRequestRoutingPolicy - chooses which resident replica of a model should
// serve the next request. Extracted so routing is independently
// unit-testable without a live WarmModelPool/PredictiveModelPool, and so
// alternate strategies (round-robin, least-latency, affinity-aware) can be
// swapped in without touching pool internals.
//
// Today every model_id has at most one resident RuntimeId (replica_id==0),
// so choose() over a single-candidate vector is a trivial passthrough. The
// seam exists so multi-replica residency (WarmModelPoolConfig::
// max_replicas_per_model > 1) plugs into existing call sites instead of
// requiring new pool-internal branching.

#pragma once

#include "qai_forge/execution/ExecutionPlan.h"
#include "qai_forge/scheduler/PriorityModelQueue.h"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace scheduler {

enum class RuntimeCandidateState {
    NotResident,
    Loading,
    Idle,
    Running,
    Draining,
    Evicting,
    Failed,
    Stopped
};

// Minimal per-replica signal the routing policy needs. Deliberately
// narrower than ModelPoolRuntimeSnapshot (WarmModelPool.h) so this header
// has no dependency on either pool's full snapshot type and can be reused
// by PredictiveModelPool's (currently single-replica) runtimes as well.
struct RuntimeCandidate {
    qai_forge::execution::RuntimeId runtime_id;
    RuntimeCandidateState state = RuntimeCandidateState::NotResident;
    size_t queue_depth = 0;
    bool has_running_job = false;
    bool healthy = false;
};

class IRequestRoutingPolicy {
public:
    virtual ~IRequestRoutingPolicy() = default;

    // Returns the chosen candidate's RuntimeId, or std::nullopt if none of
    // the candidates can currently accept work (caller should fall back to
    // creating a new replica, if under max_replicas_per_model, or queueing
    // against the sole existing one).
    virtual std::optional<qai_forge::execution::RuntimeId> choose(
        const std::vector<RuntimeCandidate>& candidates) const = 0;
};

// Default policy: prefer the resident replica with the fewest queued jobs;
// ties broken by preferring Idle over Running, then by replica_id for
// determinism. Candidates in NotResident/Failed/Stopped/Evicting are never
// selected — those states cannot accept dispatch.
class LeastQueueDepthRoutingPolicy : public IRequestRoutingPolicy {
public:
    std::optional<qai_forge::execution::RuntimeId> choose(
        const std::vector<RuntimeCandidate>& candidates) const override;
};

} // namespace scheduler
