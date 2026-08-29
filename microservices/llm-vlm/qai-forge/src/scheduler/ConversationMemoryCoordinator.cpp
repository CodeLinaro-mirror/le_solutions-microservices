// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/scheduler/ConversationMemoryCoordinator.h"

#include <utility>

namespace scheduler {

namespace {

MemoryTurnStartResult rejectedStart(
    MemoryTurnStartResult::Status status,
    std::string message) {
    MemoryTurnStartResult result;
    result.status = status;
    result.message = std::move(message);
    return result;
}

} // namespace

MemoryTurnStartResult ConversationMemoryCoordinator::beginTurn(
    const qai_forge::ConversationReference& reference) {
    if (reference.namespace_id.empty() || reference.conversation_id.empty() ||
        reference.turn_id.empty()) {
        return rejectedStart(
            MemoryTurnStartResult::Status::InvalidReference,
            "Conversation namespace, conversation ID, and turn ID are required");
    }
    if (reference.parent_policy ==
            qai_forge::ConversationParentPolicy::Explicit &&
        reference.operation == qai_forge::ConversationTurnOperation::Begin &&
        reference.parent_turn_id.empty()) {
        return rejectedStart(
            MemoryTurnStartResult::Status::InvalidReference,
            "An explicit parent policy requires parent_turn_id");
    }

    const std::string key = conversationKey(
        reference.namespace_id, reference.conversation_id);
    std::unique_lock<std::mutex> lock(mutex_);

    if (reference.operation == qai_forge::ConversationTurnOperation::Resume) {
        auto conversation_it = conversations_.find(key);
        if (conversation_it == conversations_.end()) {
            return rejectedStart(
                MemoryTurnStartResult::Status::ConversationDeleted,
                "Conversation is not available for resume");
        }
        Conversation& conversation = conversation_it->second;
        auto turn_it = conversation.turns.find(reference.turn_id);
        if (turn_it == conversation.turns.end()) {
            return rejectedStart(
                MemoryTurnStartResult::Status::ParentNotFound,
                "Conversation turn is not available for resume");
        }
        Node& node = nodes_.at(turn_it->second);
        if (node.state != MemoryTurnState::AwaitingTool) {
            return rejectedStart(
                MemoryTurnStartResult::Status::InvalidState,
                "Only an awaiting-tool turn can be resumed");
        }
        node.state = MemoryTurnState::InProgress;
        node.outcome = MemoryOutcome::None;
        conversation.last_activity = std::chrono::steady_clock::now();

        MemoryTurnStartResult result;
        result.status = MemoryTurnStartResult::Status::Resumed;
        result.token = {node.id, node.epoch};
        result.memory = nearestMemoryLocked(node.parent_id);
        return result;
    }

    while (true) {
        Conversation& conversation = conversations_[key];
        if (conversation.turns.find(reference.turn_id) !=
            conversation.turns.end()) {
            return rejectedStart(
                MemoryTurnStartResult::Status::DuplicateTurn,
                "Conversation turn already exists");
        }

        std::optional<std::uint64_t> parent_id;
        if (reference.parent_policy ==
            qai_forge::ConversationParentPolicy::Explicit) {
            auto parent_it = conversation.turns.find(reference.parent_turn_id);
            if (parent_it == conversation.turns.end()) {
                return rejectedStart(
                    MemoryTurnStartResult::Status::ParentNotFound,
                    "Explicit parent turn was not found");
            }
            parent_id = parent_it->second;
        } else {
            parent_id = conversation.latest_node_id;
        }

        if (parent_id.has_value()) {
            auto parent_it = nodes_.find(parent_id.value());
            if (parent_it == nodes_.end()) {
                return rejectedStart(
                    MemoryTurnStartResult::Status::ConversationDeleted,
                    "Conversation parent was deleted");
            }
            if (parent_it->second.state == MemoryTurnState::AwaitingTool) {
                return rejectedStart(
                    MemoryTurnStartResult::Status::InvalidState,
                    "Awaiting-tool turns must be resumed, not extended");
            }
            if (isBlocking(parent_it->second.state)) {
                cv_.wait(lock);
                continue;
            }
        }

        const std::uint64_t node_id = next_node_id_++;
        const std::uint64_t epoch = next_epoch_++;
        Node node;
        node.id = node_id;
        node.epoch = epoch;
        node.conversation_key = key;
        node.turn_id = reference.turn_id;
        node.parent_id = parent_id;
        nodes_.emplace(node_id, std::move(node));
        if (parent_id.has_value()) {
            nodes_.at(parent_id.value()).children.insert(node_id);
        }
        conversation.turns.emplace(reference.turn_id, node_id);
        conversation.latest_node_id = node_id;
        conversation.last_activity = std::chrono::steady_clock::now();

        MemoryTurnStartResult result;
        result.status = MemoryTurnStartResult::Status::Started;
        result.token = {node_id, epoch};
        result.memory = nearestMemoryLocked(parent_id);
        return result;
    }
}

bool ConversationMemoryCoordinator::beginPostTurn(
    const MemoryTurnCommitToken& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!tokenMatchesLocked(token)) {
        return false;
    }
    Node& node = nodes_.at(token.node_id);
    if (node.state != MemoryTurnState::InProgress) {
        return false;
    }
    node.state = MemoryTurnState::PostTurnPending;
    touchConversationLocked(node);
    return true;
}

bool ConversationMemoryCoordinator::markAwaitingTool(
    const MemoryTurnCommitToken& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!tokenMatchesLocked(token)) {
        return false;
    }
    Node& node = nodes_.at(token.node_id);
    if (node.state != MemoryTurnState::InProgress) {
        return false;
    }
    node.state = MemoryTurnState::AwaitingTool;
    node.outcome = MemoryOutcome::Skipped;
    touchConversationLocked(node);
    cv_.notify_all();
    return true;
}

bool ConversationMemoryCoordinator::completeStructural(
    const MemoryTurnCommitToken& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!tokenMatchesLocked(token)) {
        return false;
    }
    Node& node = nodes_.at(token.node_id);
    if (node.state != MemoryTurnState::InProgress) {
        return false;
    }
    node.state = MemoryTurnState::Ready;
    node.outcome = MemoryOutcome::Skipped;
    touchConversationLocked(node);
    cv_.notify_all();
    return true;
}

bool ConversationMemoryCoordinator::abortTurn(
    const MemoryTurnCommitToken& token,
    MemoryTurnState terminal_state) {
    if (terminal_state != MemoryTurnState::GenerationFailed &&
        terminal_state != MemoryTurnState::Cancelled) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!tokenMatchesLocked(token)) {
        return false;
    }
    Node& node = nodes_.at(token.node_id);
    if (node.state == MemoryTurnState::Ready ||
        node.state == MemoryTurnState::GenerationFailed ||
        node.state == MemoryTurnState::Cancelled) {
        return false;
    }
    node.state = terminal_state;
    node.outcome = MemoryOutcome::Failed;
    touchConversationLocked(node);
    cv_.notify_all();
    return true;
}

bool ConversationMemoryCoordinator::publish(
    const MemoryTurnCommitToken& token,
    MemoryOutcome outcome,
    std::optional<GenieMemoryState> memory) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!tokenMatchesLocked(token)) {
        return false;
    }
    Node& node = nodes_.at(token.node_id);
    if (node.state != MemoryTurnState::PostTurnPending &&
        !(outcome == MemoryOutcome::Failed &&
          node.state == MemoryTurnState::InProgress)) {
        return false;
    }
    if (outcome == MemoryOutcome::Updated && memory.has_value()) {
        node.memory = std::make_shared<const GenieMemoryState>(
            std::move(memory.value()));
    } else if (outcome == MemoryOutcome::Updated) {
        return false;
    }
    node.outcome = outcome;
    node.state = MemoryTurnState::Ready;
    touchConversationLocked(node);
    cv_.notify_all();
    return true;
}

bool ConversationMemoryCoordinator::releaseSubtree(
    const qai_forge::ConversationReference& reference) {
    const std::string key = conversationKey(
        reference.namespace_id, reference.conversation_id);
    std::lock_guard<std::mutex> lock(mutex_);
    auto conversation_it = conversations_.find(key);
    if (conversation_it == conversations_.end()) {
        return false;
    }
    Conversation& conversation = conversation_it->second;
    auto turn_it = conversation.turns.find(reference.turn_id);
    if (turn_it == conversation.turns.end()) {
        return false;
    }

    const std::uint64_t node_id = turn_it->second;
    std::optional<std::uint64_t> parent_id;
    auto node_it = nodes_.find(node_id);
    if (node_it != nodes_.end()) {
        parent_id = node_it->second.parent_id;
    }
    eraseSubtreeLocked(conversation, node_id);
    if (conversation.latest_node_id.has_value() &&
        nodes_.find(conversation.latest_node_id.value()) == nodes_.end()) {
        conversation.latest_node_id = parent_id;
    }
    if (conversation.turns.empty()) {
        conversations_.erase(conversation_it);
    }
    cv_.notify_all();
    return true;
}

bool ConversationMemoryCoordinator::releaseConversation(
    const std::string& namespace_id,
    const std::string& conversation_id) {
    const std::string key = conversationKey(namespace_id, conversation_id);
    std::lock_guard<std::mutex> lock(mutex_);
    auto conversation_it = conversations_.find(key);
    if (conversation_it == conversations_.end()) {
        return false;
    }
    for (const auto& turn : conversation_it->second.turns) {
        nodes_.erase(turn.second);
    }
    conversations_.erase(conversation_it);
    cv_.notify_all();
    return true;
}

std::size_t ConversationMemoryCoordinator::expireIdle(
    std::chrono::milliseconds ttl) {
    if (ttl.count() <= 0) {
        return 0;
    }
    const auto cutoff = std::chrono::steady_clock::now() - ttl;
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t expired = 0;
    for (auto it = conversations_.begin(); it != conversations_.end();) {
        bool has_active_turn = false;
        for (const auto& turn : it->second.turns) {
            auto node_it = nodes_.find(turn.second);
            if (node_it != nodes_.end() &&
                isBlocking(node_it->second.state)) {
                has_active_turn = true;
                break;
            }
        }
        if (has_active_turn || it->second.last_activity > cutoff) {
            ++it;
            continue;
        }
        for (const auto& turn : it->second.turns) {
            nodes_.erase(turn.second);
        }
        it = conversations_.erase(it);
        ++expired;
    }
    if (expired != 0) {
        cv_.notify_all();
    }
    return expired;
}

void ConversationMemoryCoordinator::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    conversations_.clear();
    nodes_.clear();
    legacy_entries_.clear();
    cv_.notify_all();
}

void ConversationMemoryCoordinator::seedIfAbsent(
    const std::string& memory_key,
    const ConversationMemoryUpdate& snapshot) {
    if (memory_key.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    LegacyEntry& entry = legacy_entries_[memory_key];
    if (!entry.has_snapshot) {
        entry.snapshot = snapshot;
        entry.has_snapshot = true;
    }
}

MemoryReadyResult ConversationMemoryCoordinator::awaitReady(
    const std::string& memory_key) {
    if (memory_key.empty()) {
        return {};
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (legacy_entries_.find(memory_key) == legacy_entries_.end()) {
        return {};
    }
    cv_.wait(lock, [this, &memory_key]() {
        auto found = legacy_entries_.find(memory_key);
        return found == legacy_entries_.end() || !found->second.pending;
    });
    auto found = legacy_entries_.find(memory_key);
    return found == legacy_entries_.end() ? MemoryReadyResult{}
                                          : found->second.result;
}

std::optional<ConversationMemoryUpdate>
ConversationMemoryCoordinator::committedSnapshot(
    const std::string& memory_key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = legacy_entries_.find(memory_key);
    if (found == legacy_entries_.end() || !found->second.has_snapshot) {
        return std::nullopt;
    }
    return found->second.snapshot;
}

bool ConversationMemoryCoordinator::beginPostTurn(
    const std::string& memory_key) {
    if (memory_key.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    LegacyEntry& entry = legacy_entries_[memory_key];
    if (entry.pending) {
        return false;
    }
    entry.pending = true;
    return true;
}

MemoryReadyResult ConversationMemoryCoordinator::publish(
    const std::string& memory_key,
    MemoryReadyResult::Status status,
    std::optional<ConversationMemoryUpdate> snapshot) {
    if (memory_key.empty()) {
        return {};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    LegacyEntry& entry = legacy_entries_[memory_key];
    if (!entry.pending) {
        return entry.result;
    }
    if (status == MemoryReadyResult::Status::Success && snapshot.has_value()) {
        entry.snapshot = std::move(snapshot.value());
        entry.has_snapshot = true;
    }
    entry.pending = false;
    entry.result.status = status;
    ++entry.result.snapshot_version;
    const MemoryReadyResult result = entry.result;
    cv_.notify_all();
    return result;
}

void ConversationMemoryCoordinator::cancelPending() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& item : legacy_entries_) {
        LegacyEntry& entry = item.second;
        if (entry.pending) {
            entry.pending = false;
            entry.result.status = MemoryReadyResult::Status::Cancelled;
            ++entry.result.snapshot_version;
        }
    }
    for (auto& item : nodes_) {
        Node& node = item.second;
        if (node.state == MemoryTurnState::InProgress ||
            node.state == MemoryTurnState::AwaitingTool ||
            node.state == MemoryTurnState::PostTurnPending) {
            node.state = MemoryTurnState::Cancelled;
            node.outcome = MemoryOutcome::Failed;
        }
    }
    cv_.notify_all();
}

std::string ConversationMemoryCoordinator::conversationKey(
    const std::string& namespace_id,
    const std::string& conversation_id) {
    return namespace_id + '\x1f' + conversation_id;
}

bool ConversationMemoryCoordinator::isBlocking(MemoryTurnState state) {
    return state == MemoryTurnState::InProgress ||
           state == MemoryTurnState::PostTurnPending;
}

bool ConversationMemoryCoordinator::tokenMatchesLocked(
    const MemoryTurnCommitToken& token) const {
    auto found = nodes_.find(token.node_id);
    return found != nodes_.end() && found->second.epoch == token.epoch;
}

void ConversationMemoryCoordinator::touchConversationLocked(const Node& node) {
    auto found = conversations_.find(node.conversation_key);
    if (found != conversations_.end()) {
        found->second.last_activity = std::chrono::steady_clock::now();
    }
}

std::optional<GenieMemoryState>
ConversationMemoryCoordinator::nearestMemoryLocked(
    std::optional<std::uint64_t> node_id) const {
    while (node_id.has_value()) {
        auto found = nodes_.find(node_id.value());
        if (found == nodes_.end()) {
            return std::nullopt;
        }
        if (found->second.memory) {
            return *found->second.memory;
        }
        node_id = found->second.parent_id;
    }
    return std::nullopt;
}

void ConversationMemoryCoordinator::eraseSubtreeLocked(
    Conversation& conversation,
    std::uint64_t node_id) {
    auto node_it = nodes_.find(node_id);
    if (node_it == nodes_.end()) {
        return;
    }
    const std::vector<std::uint64_t> children(
        node_it->second.children.begin(), node_it->second.children.end());
    for (const std::uint64_t child_id : children) {
        eraseSubtreeLocked(conversation, child_id);
    }
    if (node_it->second.parent_id.has_value()) {
        auto parent_it = nodes_.find(node_it->second.parent_id.value());
        if (parent_it != nodes_.end()) {
            parent_it->second.children.erase(node_id);
        }
    }
    conversation.turns.erase(node_it->second.turn_id);
    nodes_.erase(node_it);
}

} // namespace scheduler
