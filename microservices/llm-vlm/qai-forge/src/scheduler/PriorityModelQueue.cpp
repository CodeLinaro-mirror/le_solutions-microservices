// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/scheduler/PriorityModelQueue.h"

#include "qai_forge/utils/Logger.h"

#include <stdexcept>
#include <utility>

namespace scheduler {

namespace {

const char* priorityToString(JobPriority priority) {
    switch (priority) {
        case JobPriority::CONTROL:
            return "control";
        case JobPriority::TOOL_CONTINUATION:
            return "tool_continuation";
        case JobPriority::ANY_REQUEST:
            return "any_request";
    }
    return "unknown";
}

} // namespace

void PriorityModelQueue::push(GenerativeJobPtr job) {
    if (!job) {
        throw std::invalid_argument("PriorityModelQueue::push received null job");
    }

    const std::string job_id = job->job_id;
    const JobPriority priority = job->priority;
    std::lock_guard<std::mutex> lock(mutex_);
    laneFor(job->priority).push_back(std::move(job));
    LOG_INFO("[PriorityModelQueue] Pushed job: job=" << job_id
             << " priority=" << priorityToString(priority));
}

GenerativeJobPtr PriorityModelQueue::pop() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (auto job = popFrom(control_)) {
        LOG_INFO("[PriorityModelQueue] Popped job: job=" << job->job_id
                 << " priority=control");
        return job;
    }
    if (auto job = popFrom(tool_continuation_)) {
        LOG_INFO("[PriorityModelQueue] Popped job: job=" << job->job_id
                 << " priority=tool_continuation");
        return job;
    }
    auto job = popFrom(any_request_);
    if (job) {
        LOG_INFO("[PriorityModelQueue] Popped job: job=" << job->job_id
                 << " priority=any_request");
    }
    return job;
}

GenerativeJobPtr PriorityModelQueue::cancel(const std::string& job_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto cancel_from = [&job_id](Lane& lane) -> GenerativeJobPtr {
        for (auto it = lane.begin(); it != lane.end(); ++it) {
            const GenerativeJobPtr& job = *it;
            if (job && job->job_id == job_id) {
                GenerativeJobPtr cancelled = job;
                cancelled->cancel();
                lane.erase(it);
                LOG_INFO("[PriorityModelQueue] Cancelled queued job: job="
                         << job_id);
                return cancelled;
            }
        }
        return nullptr;
    };

    if (auto job = cancel_from(control_)) return job;
    if (auto job = cancel_from(tool_continuation_)) return job;
    return cancel_from(any_request_);
}

bool PriorityModelQueue::empty() const {
    return size() == 0;
}

size_t PriorityModelQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return control_.size() + tool_continuation_.size() + any_request_.size();
}

size_t PriorityModelQueue::size(JobPriority priority) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return laneFor(priority).size();
}

QueueSnapshot PriorityModelQueue::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return QueueSnapshot{
        control_.size(),
        tool_continuation_.size(),
        any_request_.size(),
    };
}

QueueAdmissionCandidate PriorityModelQueue::admissionCandidate() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (auto candidate = peekFrom(control_, JobPriority::CONTROL);
        candidate.has_work) {
        return candidate;
    }
    if (auto candidate = peekFrom(tool_continuation_,
                                  JobPriority::TOOL_CONTINUATION);
        candidate.has_work) {
        return candidate;
    }
    return peekFrom(any_request_, JobPriority::ANY_REQUEST);
}

PriorityModelQueue::Lane& PriorityModelQueue::laneFor(JobPriority priority) {
    switch (priority) {
        case JobPriority::CONTROL:
            return control_;
        case JobPriority::TOOL_CONTINUATION:
            return tool_continuation_;
        case JobPriority::ANY_REQUEST:
            return any_request_;
    }

    return any_request_;
}

const PriorityModelQueue::Lane& PriorityModelQueue::laneFor(JobPriority priority) const {
    switch (priority) {
        case JobPriority::CONTROL:
            return control_;
        case JobPriority::TOOL_CONTINUATION:
            return tool_continuation_;
        case JobPriority::ANY_REQUEST:
            return any_request_;
    }

    return any_request_;
}

GenerativeJobPtr PriorityModelQueue::popFrom(Lane& lane) {
    while (!lane.empty()) {
        GenerativeJobPtr job = std::move(lane.front());
        lane.pop_front();

        if (!job || job->isCancelled()) {
            continue;
        }

        return job;
    }

    return nullptr;
}

QueueAdmissionCandidate PriorityModelQueue::peekFrom(
    const Lane& lane,
    JobPriority priority) const {
    for (const GenerativeJobPtr& job : lane) {
        if (!job || job->isCancelled()) {
            continue;
        }

        return QueueAdmissionCandidate{
            true,
            priority,
            job->created_at,
            job->job_id,
        };
    }

    return QueueAdmissionCandidate{};
}

} // namespace scheduler
