// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/scheduler/ConversationMemoryCoordinator.h"
#include "qai_forge/scheduler/GenerativeJob.h"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scheduler {

// Tracks in-flight/resumable requests by response_id for cancellation and
// internal-MCP-round resumption. An "active operation" models one logical
// response_id across its possibly multiple scheduler job submissions
// (internal MCP rounds resubmit under the same response_id with a fresh
// job_id each round).
class ActiveOperationRegistry {
public:
    struct Operation {
        std::string job_id;
        std::string lease_model_id;
        std::string lease_id;
        // Captured from GenerativeJob::session_id in registerOperation() so
        // markCancelled()/expireIdle() can target affinity-aware
        // closeChain() calls instead of falling back to a broadcast.
        std::string session_id;
        std::optional<MemoryTurnCommitToken> memory_turn;
        bool cancelled = false;
        std::chrono::steady_clock::time_point last_activity =
            std::chrono::steady_clock::now();
    };

    // Snapshot returned by markCancelled() — carries just enough state for
    // the caller to close any associated tool-chain lease and abort any
    // associated conversation-memory turn.
    struct CancelInfo {
        bool found = false;
        std::string job_id;
        std::string lease_model_id;
        std::string lease_id;
        std::string session_id;
        std::optional<MemoryTurnCommitToken> memory_turn;
    };

    // Registers a newly-submitted job under its response_id. Throws
    // GenAIException(409) if response_id already has a different active
    // job_id, or was previously cancelled.
    void registerOperation(const GenerativeJob& job);

    // Marks the operation as finished. If keep_for_resume is true (and the
    // operation was not cancelled), the entry is retained with an empty
    // job_id so a later internal-MCP-round resubmission under the same
    // response_id can re-register — returns true in that case. Otherwise
    // erases the entry and returns false.
    bool finishOperation(const std::string& response_id,
                         const std::string& job_id,
                         bool keep_for_resume);

    // Marks response_id as cancelled and returns a snapshot of its lease/
    // memory-turn state for the caller to clean up. CancelInfo::found is
    // false if response_id has no active operation.
    CancelInfo markCancelled(const std::string& response_id);

    // Sweeps operations kept for resume (empty job_id) whose last_activity
    // predates `ttl` ago, and returns their lease identities for the
    // caller to close.
    std::vector<std::pair<std::string, std::string>> expireIdle(
        std::chrono::milliseconds ttl);

private:
    std::mutex mutex_;
    std::unordered_map<std::string, Operation> operations_;
};

} // namespace scheduler
