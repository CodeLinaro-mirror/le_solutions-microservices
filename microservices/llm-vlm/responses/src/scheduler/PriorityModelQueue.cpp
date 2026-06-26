// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "scheduler/PriorityModelQueue.h"

#include "qai_forge/utils/Logger.h"

#include <stdexcept>
#include <utility>

namespace scheduler {

namespace {

const char* priorityToString(JobPriority priority) {
    switch (priority) {
        case JobPriority::CONTROL:
            return "control";
        case JobPriority::READY_TOOL_CONT:
            return "ready_tool_cont";
        case JobPriority::SESSION_CONT:
            return "session_cont";
        case JobPriority::NEW_REQUEST:
            return "new_request";
    }
    return "unknown";
}

} // namespace

void PriorityModelQueue::push(InferenceJobPtr job) {
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

InferenceJobPtr PriorityModelQueue::pop() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (auto job = popFrom(control_)) {
        LOG_INFO("[PriorityModelQueue] Popped job: job=" << job->job_id
                 << " priority=control");
        return job;
    }
    if (auto job = popFrom(ready_tool_cont_)) {
        LOG_INFO("[PriorityModelQueue] Popped job: job=" << job->job_id
                 << " priority=ready_tool_cont");
        return job;
    }
    if (auto job = popFrom(session_cont_)) {
        LOG_INFO("[PriorityModelQueue] Popped job: job=" << job->job_id
                 << " priority=session_cont");
        return job;
    }
    auto job = popFrom(new_request_);
    if (job) {
        LOG_INFO("[PriorityModelQueue] Popped job: job=" << job->job_id
                 << " priority=new_request");
    }
    return job;
}

InferenceJobPtr PriorityModelQueue::popProtectedDrainJob() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (auto job = popFrom(control_)) {
        LOG_INFO("[PriorityModelQueue] Popped protected-drain job: job="
                 << job->job_id << " priority=control");
        return job;
    }
    auto job = popFrom(ready_tool_cont_);
    if (job) {
        LOG_INFO("[PriorityModelQueue] Popped protected-drain job: job="
                 << job->job_id << " priority=ready_tool_cont");
    }
    return job;
}

InferenceJobPtr PriorityModelQueue::cancel(const std::string& job_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto cancel_from = [&job_id](Lane& lane) -> InferenceJobPtr {
        for (auto it = lane.begin(); it != lane.end(); ++it) {
            const InferenceJobPtr& job = *it;
            if (job && job->job_id == job_id) {
                InferenceJobPtr cancelled = job;
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
    if (auto job = cancel_from(ready_tool_cont_)) return job;
    if (auto job = cancel_from(session_cont_)) return job;
    return cancel_from(new_request_);
}

bool PriorityModelQueue::empty() const {
    return size() == 0;
}

bool PriorityModelQueue::hasProtectedDrainWork() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hasReadyJobIn(control_) || hasReadyJobIn(ready_tool_cont_);
}

size_t PriorityModelQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return control_.size() + ready_tool_cont_.size() +
           session_cont_.size() + new_request_.size();
}

size_t PriorityModelQueue::size(JobPriority priority) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return laneFor(priority).size();
}

QueueSnapshot PriorityModelQueue::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return QueueSnapshot{
        control_.size(),
        ready_tool_cont_.size(),
        session_cont_.size(),
        new_request_.size(),
    };
}

QueueAdmissionCandidate PriorityModelQueue::admissionCandidate() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (auto candidate = peekFrom(control_, JobPriority::CONTROL);
        candidate.has_work) {
        return candidate;
    }
    if (auto candidate = peekFrom(ready_tool_cont_,
                                  JobPriority::READY_TOOL_CONT);
        candidate.has_work) {
        return candidate;
    }
    if (auto candidate = peekFrom(session_cont_, JobPriority::SESSION_CONT);
        candidate.has_work) {
        return candidate;
    }
    return peekFrom(new_request_, JobPriority::NEW_REQUEST);
}

size_t PriorityModelQueue::promoteAgedNewRequests(
    std::chrono::steady_clock::time_point now,
    std::chrono::milliseconds threshold) {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t promoted_count = 0;
    Lane remaining_new_requests;

    while (!new_request_.empty()) {
        InferenceJobPtr job = std::move(new_request_.front());
        new_request_.pop_front();

        if (!job) {
            continue;
        }

        if (job->isCancelled()) {
            continue;
        }

        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - job->created_at);
        if (age >= threshold) {
            job->priority = JobPriority::SESSION_CONT;
            LOG_INFO("[PriorityModelQueue] Promoted aged job: job="
                     << job->job_id << " from=new_request to=session_cont"
                     << " age_ms=" << age.count());
            session_cont_.push_back(std::move(job));
            ++promoted_count;
        } else {
            remaining_new_requests.push_back(std::move(job));
        }
    }

    new_request_ = std::move(remaining_new_requests);
    return promoted_count;
}

PriorityModelQueue::Lane& PriorityModelQueue::laneFor(JobPriority priority) {
    switch (priority) {
        case JobPriority::CONTROL:
            return control_;
        case JobPriority::READY_TOOL_CONT:
            return ready_tool_cont_;
        case JobPriority::SESSION_CONT:
            return session_cont_;
        case JobPriority::NEW_REQUEST:
            return new_request_;
    }

    return new_request_;
}

const PriorityModelQueue::Lane& PriorityModelQueue::laneFor(JobPriority priority) const {
    switch (priority) {
        case JobPriority::CONTROL:
            return control_;
        case JobPriority::READY_TOOL_CONT:
            return ready_tool_cont_;
        case JobPriority::SESSION_CONT:
            return session_cont_;
        case JobPriority::NEW_REQUEST:
            return new_request_;
    }

    return new_request_;
}

InferenceJobPtr PriorityModelQueue::popFrom(Lane& lane) {
    while (!lane.empty()) {
        InferenceJobPtr job = std::move(lane.front());
        lane.pop_front();

        if (!job || job->isCancelled()) {
            continue;
        }

        return job;
    }

    return nullptr;
}

bool PriorityModelQueue::hasReadyJobIn(const Lane& lane) const {
    for (const InferenceJobPtr& job : lane) {
        if (job && !job->isCancelled()) {
            return true;
        }
    }
    return false;
}

QueueAdmissionCandidate PriorityModelQueue::peekFrom(
    const Lane& lane,
    JobPriority priority) const {
    for (const InferenceJobPtr& job : lane) {
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
