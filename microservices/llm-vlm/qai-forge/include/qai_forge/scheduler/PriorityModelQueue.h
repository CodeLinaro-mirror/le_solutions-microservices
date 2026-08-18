// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/scheduler/GenerativeJob.h"

#include <chrono>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>

namespace scheduler {

struct QueueSnapshot {
    size_t control = 0;
    size_t tool_continuation = 0;
    size_t any_request = 0;

    size_t total() const {
        return control + tool_continuation + any_request;
    }
};

struct QueueAdmissionCandidate {
    bool has_work = false;
    JobPriority priority = JobPriority::ANY_REQUEST;
    std::chrono::steady_clock::time_point created_at =
        std::chrono::steady_clock::time_point::max();
    std::string job_id;
};

// Per-model priority queue. It preserves FIFO ordering within each priority
// lane and always pops from the highest-priority non-empty lane.
class PriorityModelQueue {
public:
    void push(GenerativeJobPtr job);
    GenerativeJobPtr pop();
    GenerativeJobPtr cancel(const std::string& job_id);

    bool empty() const;
    size_t size() const;
    size_t size(JobPriority priority) const;
    QueueSnapshot snapshot() const;
    QueueAdmissionCandidate admissionCandidate() const;

private:
    using Lane = std::deque<GenerativeJobPtr>;

    Lane& laneFor(JobPriority priority);
    const Lane& laneFor(JobPriority priority) const;
    GenerativeJobPtr popFrom(Lane& lane);
    QueueAdmissionCandidate peekFrom(const Lane& lane,
                                     JobPriority priority) const;

    mutable std::mutex mutex_;
    Lane control_;
    Lane tool_continuation_;
    Lane any_request_;
};

} // namespace scheduler
