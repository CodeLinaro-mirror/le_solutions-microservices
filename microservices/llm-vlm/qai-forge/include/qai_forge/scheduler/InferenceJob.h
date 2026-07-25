// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/InternalDTOs.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace scheduler {

// Queue priority is ordered from most urgent to least urgent.
enum class JobPriority {
    CONTROL = 0,
    READY_TOOL_CONT = 1,
    SESSION_CONT = 2,
    NEW_REQUEST = 3,
};

enum class JobKind {
    HTTP_NON_STREAMING,
    HTTP_STREAMING,
    WEBSOCKET,
    MCP_ROUND,
    INTERNAL_SUMMARIZATION,
};

struct SchedulerInvokeOptions {
    std::string response_id;
    std::string previous_response_id;
    std::string session_id;
    JobKind kind = JobKind::HTTP_NON_STREAMING;
    JobPriority priority = JobPriority::NEW_REQUEST;
    bool tool_output_submission = false;
    bool allow_tool_chain_fallback = false;
    bool skip_summarization_middleware = false;
    bool use_response_history = false;
    json response_history = json::array();

    std::string summary_content;
    int summary_token_count = 0;
    std::unordered_map<std::string, std::string> facts;
    std::size_t evicted_message_count = 0;
};

enum class SubmitStatus {
    QUEUED,
    REJECTED_MODEL_NOT_FOUND,
    REJECTED_QUEUE_FULL,
    REJECTED_ADMISSION_TIMEOUT,
    REJECTED_SHUTTING_DOWN,
    REJECTED_PREVIOUS_RESPONSE_NOT_FOUND,
    REJECTED_TOOL_RESPONSE_TIMEOUT,
};

struct SubmitResult {
    SubmitStatus status = SubmitStatus::QUEUED;
    std::string job_id;
    std::string message;

    bool accepted() const {
        return status == SubmitStatus::QUEUED;
    }
};

// Transport-owned callbacks. The scheduler and runner only invoke these;
// controllers remain responsible for HTTP/SSE/WebSocket wire formatting.
struct InferenceCallbacks {
    std::function<void(const StreamChunk&)> on_token;
    std::function<void(const StandardResponse&)> on_complete;
    std::function<void(const GenAIException&)> on_error;
    std::function<void()> on_cancelled;
};

// One model inference request submitted to ModelScheduler.
//
// This wraps CreateChatCompletionRequest with scheduler metadata: identity,
// priority, timing, cancellation, and transport-owned callbacks. It must not
// hold Drogon request/response/stream/websocket objects directly.
struct InferenceJob {
    std::string job_id;
    std::string response_id;
    std::string previous_response_id;
    std::string session_id;
    std::string model_id;
    std::string tool_chain_id;

    JobKind kind = JobKind::HTTP_NON_STREAMING;
    JobPriority priority = JobPriority::NEW_REQUEST;
    bool is_tool_output_submission = false;
    bool is_tool_continuation = false;
    bool allow_tool_chain_fallback = false;
    std::shared_ptr<const SchedulerInvokeOptions> invoke_options;

    CreateChatCompletionRequest request;

    std::chrono::steady_clock::time_point created_at =
        std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point enqueue_deadline =
        std::chrono::steady_clock::time_point::max();

    std::atomic<bool> cancelled{false};
    std::atomic<bool> cancellation_notified{false};
    InferenceCallbacks callbacks;

    InferenceJob() = default;
    ~InferenceJob() = default;

    InferenceJob(const InferenceJob&) = delete;
    InferenceJob& operator=(const InferenceJob&) = delete;

    InferenceJob(InferenceJob&&) = delete;
    InferenceJob& operator=(InferenceJob&&) = delete;

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

using InferenceJobPtr = std::shared_ptr<InferenceJob>;

} // namespace scheduler
