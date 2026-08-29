// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/QaiForge.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace scheduler {

enum class MemoryTurnState {
    InProgress,
    AwaitingTool,
    PostTurnPending,
    Ready,
    GenerationFailed,
    Cancelled,
    Deleted,
};

enum class MemoryOutcome {
    None,
    Updated,
    Unchanged,
    Skipped,
    Failed,
};

struct GenieMemoryState {
    std::string summary_content;
    int summary_token_count = 0;
    std::unordered_map<std::string, std::string> facts;
    std::size_t canonical_boundary = 0;
    std::string transcript_digest;
};

// Private scheduler capability. Publication performs lookup only and verifies
// both values, so stale work cannot recreate or update a deleted/reused turn.
struct MemoryTurnCommitToken {
    std::uint64_t node_id = 0;
    std::uint64_t epoch = 0;

    bool valid() const {
        return node_id != 0 && epoch != 0;
    }
};

struct MemoryTurnStartResult {
    enum class Status {
        Started,
        Resumed,
        InvalidReference,
        ParentNotFound,
        DuplicateTurn,
        InvalidState,
        ConversationDeleted,
    };

    Status status = Status::InvalidReference;
    MemoryTurnCommitToken token;
    std::optional<GenieMemoryState> memory;
    std::string message;

    bool accepted() const {
        return status == Status::Started || status == Status::Resumed;
    }
};

// Owns the private runtime-memory tree. Public API stores retain only opaque
// conversation/turn identifiers and never receive Genie memory contents.
class ConversationMemoryCoordinator {
public:
    MemoryTurnStartResult beginTurn(
        const qai_forge::ConversationReference& reference);
    bool beginPostTurn(const MemoryTurnCommitToken& token);
    bool markAwaitingTool(const MemoryTurnCommitToken& token);
    bool completeStructural(const MemoryTurnCommitToken& token);
    bool abortTurn(const MemoryTurnCommitToken& token,
                   MemoryTurnState terminal_state);
    bool publish(const MemoryTurnCommitToken& token,
                 MemoryOutcome outcome,
                 std::optional<GenieMemoryState> memory = std::nullopt);

    bool releaseSubtree(const qai_forge::ConversationReference& reference);
    bool releaseConversation(const std::string& namespace_id,
                             const std::string& conversation_id);
    std::size_t expireIdle(std::chrono::milliseconds ttl);
    void clear();

    void cancelPending();

private:
    struct Node {
        std::uint64_t id = 0;
        std::uint64_t epoch = 0;
        std::string conversation_key;
        std::string turn_id;
        std::optional<std::uint64_t> parent_id;
        std::unordered_set<std::uint64_t> children;
        MemoryTurnState state = MemoryTurnState::InProgress;
        MemoryOutcome outcome = MemoryOutcome::None;
        std::shared_ptr<const GenieMemoryState> memory;
    };

    struct Conversation {
        std::unordered_map<std::string, std::uint64_t> turns;
        std::optional<std::uint64_t> latest_node_id;
        std::chrono::steady_clock::time_point last_activity =
            std::chrono::steady_clock::now();
    };

    static std::string conversationKey(const std::string& namespace_id,
                                       const std::string& conversation_id);
    static bool isBlocking(MemoryTurnState state);
    bool tokenMatchesLocked(const MemoryTurnCommitToken& token) const;
    void touchConversationLocked(const Node& node);
    std::optional<GenieMemoryState> nearestMemoryLocked(
        std::optional<std::uint64_t> node_id) const;
    void eraseSubtreeLocked(Conversation& conversation,
                            std::uint64_t node_id);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<std::string, Conversation> conversations_;
    std::unordered_map<std::uint64_t, Node> nodes_;
    std::uint64_t next_node_id_ = 1;
    std::uint64_t next_epoch_ = 1;
};

} // namespace scheduler
