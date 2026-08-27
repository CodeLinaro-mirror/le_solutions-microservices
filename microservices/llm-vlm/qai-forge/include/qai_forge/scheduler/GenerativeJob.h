// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/InternalDTOs.h"
#include "qai_forge/scheduler/GenerativeJobContext.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace scheduler {

struct GenerativeCallbacks {
    std::function<void(const StreamChunk&)> on_token;
    std::function<void(const StandardResponse&)> on_complete;
    std::function<void(const GenAIException&)> on_error;
    std::function<void()> on_cancelled;
};

struct PostTurnInput {
    std::string session_id;
    std::string conversation_memory_key;
    json request_messages = json::array();
};

struct PostTurnTask {
    PostTurnInput input;
    StandardResponse response;
};

struct GenerativeJob {
    std::string job_id;
    std::string model_id;
    std::string response_id;
    std::string session_id;
    std::string tool_chain_id;
    std::string conversation_memory_key;

    JobKind kind = JobKind::HTTP_NON_STREAMING;
    JobPriority priority = JobPriority::NEW_REQUEST;
    PreparedGenerativeRequest prepared;
    bool skip_post_turn_summarization = false;

    std::chrono::steady_clock::time_point created_at =
        std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point enqueue_deadline =
        std::chrono::steady_clock::time_point::max();

    std::atomic<bool> cancelled{false};
    std::atomic<bool> cancellation_notified{false};
    GenerativeCallbacks callbacks;

    GenerativeJob() = default;
    ~GenerativeJob() = default;

    GenerativeJob(const GenerativeJob&) = delete;
    GenerativeJob& operator=(const GenerativeJob&) = delete;
    GenerativeJob(GenerativeJob&&) = delete;
    GenerativeJob& operator=(GenerativeJob&&) = delete;

    bool isCancelled() const {
        return cancelled.load(std::memory_order_relaxed);
    }

    void cancel() {
        cancelled.store(true, std::memory_order_relaxed);
    }

    bool markCancellationNotified() {
        bool expected = false;
        return cancellation_notified.compare_exchange_strong(
            expected, true, std::memory_order_relaxed);
    }
};

using GenerativeJobPtr = std::shared_ptr<GenerativeJob>;

} // namespace scheduler
