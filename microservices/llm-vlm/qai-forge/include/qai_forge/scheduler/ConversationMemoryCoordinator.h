// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/InternalDTOs.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace scheduler {

struct MemoryReadyResult {
    enum class Status {
        Success,
        Failed,
        Cancelled,
    };

    Status status = Status::Success;
    std::uint64_t snapshot_version = 0;
};

// Coordinates a committed in-memory conversation snapshot and its readiness.
// The key is a logical conversation or response-lineage node. Model inference
// and durable persistence remain elsewhere.
class ConversationMemoryCoordinator {
public:
    void seedIfAbsent(const std::string& memory_key,
                      const ConversationMemoryUpdate& snapshot);
    MemoryReadyResult awaitReady(const std::string& memory_key);
    std::optional<ConversationMemoryUpdate> committedSnapshot(
        const std::string& memory_key) const;

    bool beginPostTurn(const std::string& memory_key);
    MemoryReadyResult publish(
        const std::string& memory_key,
        MemoryReadyResult::Status status,
        std::optional<ConversationMemoryUpdate> snapshot = std::nullopt);
    void cancelPending();

private:
    struct Entry {
        ConversationMemoryUpdate snapshot;
        bool has_snapshot = false;
        bool pending = false;
        MemoryReadyResult result;
    };

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<std::string, Entry> entries_;
};

} // namespace scheduler
