// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/InternalDTOs.h"
#include "qai_forge/orchestration/IGenerativeOrchestrator.h"
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
    MemoryTurnCommitToken memory_turn;
    // The memory snapshot fed into IGenerativeOrchestrator::prepare() for
    // this job, translated to GenieMemoryState. OrchestratorAdapter uses
    // this as the "before" value to detect whether postTurn() actually
    // changed anything (postTurn() returning std::nullopt means "no
    // change" — the adapter echoes this value back unchanged so
    // PostTurnWorker's memoryChanged() comparison correctly reports
    // MemoryOutcome::Unchanged rather than treating a no-op as a change).
    GenieMemoryState before_memory;
    // Opaque state produced by IGenerativeOrchestrator::prepare() for this
    // job, carried forward so postTurn() can inspect/reuse whatever the
    // orchestrator stashed in it (e.g. the full conversation_messages for
    // the completed turn). Replaces the old request_messages/input_memory
    // fields — OrchestratorAdapter now owns translating memory snapshots.
    std::unique_ptr<qai_forge::PreparedGenerationState> prepared_state;
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
