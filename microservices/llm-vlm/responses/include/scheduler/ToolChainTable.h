// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace scheduler {

enum class ToolChainState {
    WaitingForToolResult,
    ContinuationQueued,
};

enum class ToolChainResolveStatus {
    Found,
    NotFound,
    Expired,
};

struct ToolChainEntry {
    std::string chain_id;
    std::string response_id;
    std::string model_id;
    std::string session_id;
    std::string pending_job_id;
    ToolChainState state = ToolChainState::WaitingForToolResult;
    bool awaiting_tool_result = true;
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point ttl_deadline;
};

struct ToolChainResolveResult {
    ToolChainResolveStatus status = ToolChainResolveStatus::NotFound;
    std::optional<ToolChainEntry> entry;
    std::string message;
};

// Tracks response_id -> pending external tool-call chains.
//
// Normal session continuations also use previous_response_id, so callers should
// only resolve through this table when the incoming request actually carries
// tool output.
class ToolChainTable {
public:
    explicit ToolChainTable(
        std::chrono::milliseconds expired_record_retention =
            std::chrono::milliseconds::max());

    ToolChainEntry open(const std::string& response_id,
                        const std::string& model_id,
                        const std::string& session_id,
                        std::chrono::milliseconds ttl,
                        std::chrono::steady_clock::time_point now =
                            std::chrono::steady_clock::now());

    ToolChainResolveResult resolveByPreviousResponseId(
        const std::string& previous_response_id,
        std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now());

    bool markContinuationQueued(const std::string& chain_id,
                                const std::string& job_id,
                                std::chrono::milliseconds ttl,
                                std::chrono::steady_clock::time_point now =
                                    std::chrono::steady_clock::now());

    bool close(const std::string& chain_id);

    std::vector<ToolChainEntry> expireStale(
        std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now());

    std::vector<ToolChainEntry> snapshot() const;
    size_t size() const;

private:
    struct ExpiredRecord {
        std::chrono::steady_clock::time_point expired_at;
        std::chrono::steady_clock::time_point forget_at;
    };

    using ChainMap = std::unordered_map<std::string, ToolChainEntry>;

    std::string makeChainIdLocked(const std::string& response_id);
    void removeLiveLocked(ChainMap::iterator it);
    ToolChainEntry moveToExpiredLocked(ChainMap::iterator it,
                                       std::chrono::steady_clock::time_point now);
    void cleanupExpiredRecordsLocked(std::chrono::steady_clock::time_point now);

    std::chrono::milliseconds expired_record_retention_;

    mutable std::mutex mutex_;
    ChainMap chains_by_id_;
    std::unordered_map<std::string, std::string> chain_id_by_response_id_;
    std::unordered_map<std::string, ExpiredRecord> expired_response_ids_;
    uint64_t next_generated_id_ = 1;
};

} // namespace scheduler
