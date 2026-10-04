// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/InternalDTOs.h"
#include "qai_forge/orchestration/IGenerativeOrchestrator.h"
#include "qai_forge/scheduler/ConversationMemoryCoordinator.h"
#include "qai_forge/scheduler/GenerativeJobContext.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace scheduler {

struct GenerativeCallbacks {
    std::function<void(const StreamChunk&)> on_token;
    std::function<void(const StandardResponse&)> on_complete;
    std::function<void(const GenAIException&)> on_error;
    std::function<void()> on_cancelled;
};

// The scheduler (ModelRuntime/PostTurnWorker) only ever deals with the
// opaque PostTurnWork interface (see OrchestratorAdapter.h) returned from
// OrchestratorAdapter::createPostTurnTask().

struct GenerativeJob {
    std::string job_id;
    std::string model_id;
    std::string response_id;
    std::string session_id;
    std::string tool_chain_id;
    std::optional<MemoryTurnCommitToken> memory_turn;
    // The memory snapshot this job's prepare() call was given, carried
    // forward so OrchestratorAdapter::createPostTurnTask() can copy it into
    // PostTurnInput::before_memory for the "did postTurn() actually change
    // anything" comparison. Populated by OrchestratorAdapter::createJob().
    GenieMemoryState initial_memory_state;

    JobKind kind = JobKind::HTTP_NON_STREAMING;
    JobPriority priority = JobPriority::ANY_REQUEST;
    // Opaque state produced by IGenerativeOrchestrator::prepare(). Replaces
    // the old closed std::variant<GeniePreparedRequest, LlamaCppPreparedRequest,
    // LiteRTLMPreparedRequest> — see PreparedGenerationState's doc comment.
    std::unique_ptr<qai_forge::PreparedGenerationState> prepared_state;
    bool skip_post_turn_summarization = false;

    std::chrono::steady_clock::time_point created_at =
        std::chrono::steady_clock::now();
    // Set by WarmModelPool::submit() to created_at + queue_admission_timeout
    // when WarmModelPoolConfig::queue_admission_timeout > 0 (0/default =
    // disabled, this field stays time_point::max()). Enforced by
    // ModelRuntime::expireStaleQueuedJobs() / PriorityModelQueue::
    // expireOlderThan(), which proactively fail (via on_error,
    // INSUFFICIENT_MEMORY/503) any job still queued past this deadline
    // instead of letting it eventually run for a caller that may have
    // long since given up.
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
