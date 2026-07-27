// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/scheduler/InferenceJob.h"

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>

namespace scheduler {

struct QueueSnapshot {
    size_t control = 0;
    size_t ready_tool_cont = 0;
    size_t session_cont = 0;
    size_t new_request = 0;

    size_t total() const {
        return control + ready_tool_cont + session_cont + new_request;
    }
};

struct QueueAdmissionCandidate {
    bool has_work = false;
    JobPriority priority = JobPriority::NEW_REQUEST;
    std::chrono::steady_clock::time_point created_at =
        std::chrono::steady_clock::time_point::max();
    std::string job_id;
};

// Per-model priority queue. It preserves FIFO ordering within each priority
// lane and always pops from the highest-priority non-empty lane.
class PriorityModelQueue {
public:
    void push(InferenceJobPtr job);
    InferenceJobPtr pop();
    InferenceJobPtr cancel(const std::string& job_id);

    bool empty() const;
    size_t size() const;
    size_t size(JobPriority priority) const;
    QueueSnapshot snapshot() const;
    QueueAdmissionCandidate admissionCandidate() const;

    // Promote aged NEW_REQUEST jobs to SESSION_CONT. Tool continuations are
    // intentionally unaffected and always keep priority over aged new requests.
    size_t promoteAgedNewRequests(std::chrono::steady_clock::time_point now,
                                  std::chrono::milliseconds threshold);

private:
    using Lane = std::deque<InferenceJobPtr>;

    Lane& laneFor(JobPriority priority);
    const Lane& laneFor(JobPriority priority) const;
    InferenceJobPtr popFrom(Lane& lane);
    QueueAdmissionCandidate peekFrom(const Lane& lane,
                                     JobPriority priority) const;

    mutable std::mutex mutex_;
    Lane control_;
    Lane ready_tool_cont_;
    Lane session_cont_;
    Lane new_request_;
};

} // namespace scheduler
