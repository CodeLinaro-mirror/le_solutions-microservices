// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/scheduler/IRequestRoutingPolicy.h"

namespace scheduler {

namespace {

bool canAcceptDispatch(RuntimeCandidateState state) {
    return state == RuntimeCandidateState::Idle ||
           state == RuntimeCandidateState::Running;
}

// Lower is better: Idle preferred over Running when queue depths tie.
int statePriority(RuntimeCandidateState state) {
    switch (state) {
        case RuntimeCandidateState::Idle:
            return 0;
        case RuntimeCandidateState::Running:
            return 1;
        default:
            return 2;
    }
}

} // namespace

std::optional<qai_forge::execution::RuntimeId>
LeastQueueDepthRoutingPolicy::choose(
    const std::vector<RuntimeCandidate>& candidates) const {
    const RuntimeCandidate* best = nullptr;

    for (const RuntimeCandidate& candidate : candidates) {
        if (!canAcceptDispatch(candidate.state) || !candidate.healthy) {
            continue;
        }
        if (!best) {
            best = &candidate;
            continue;
        }
        if (candidate.queue_depth != best->queue_depth) {
            if (candidate.queue_depth < best->queue_depth) {
                best = &candidate;
            }
            continue;
        }
        if (statePriority(candidate.state) != statePriority(best->state)) {
            if (statePriority(candidate.state) < statePriority(best->state)) {
                best = &candidate;
            }
            continue;
        }
        if (candidate.runtime_id.replica_id < best->runtime_id.replica_id) {
            best = &candidate;
        }
    }

    if (!best) {
        return std::nullopt;
    }
    return best->runtime_id;
}

} // namespace scheduler
