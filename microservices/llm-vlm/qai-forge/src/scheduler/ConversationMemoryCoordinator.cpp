// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/scheduler/ConversationMemoryCoordinator.h"

namespace scheduler {

void ConversationMemoryCoordinator::seedIfAbsent(
    const std::string& memory_key,
    const ConversationMemoryUpdate& snapshot) {
    if (memory_key.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    Entry& entry = entries_[memory_key];
    if (entry.has_snapshot) {
        return;
    }
    entry.snapshot = snapshot;
    entry.has_snapshot = true;
}

MemoryReadyResult ConversationMemoryCoordinator::awaitReady(
    const std::string& memory_key) {
    if (memory_key.empty()) {
        return {};
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (entries_.find(memory_key) == entries_.end()) {
        return {};
    }
    cv_.wait(lock, [this, &memory_key]() {
        auto found = entries_.find(memory_key);
        return found == entries_.end() || !found->second.pending;
    });
    auto found = entries_.find(memory_key);
    if (found == entries_.end()) {
        return {};
    }
    return found->second.result;
}

std::optional<ConversationMemoryUpdate>
ConversationMemoryCoordinator::committedSnapshot(
    const std::string& memory_key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = entries_.find(memory_key);
    if (found == entries_.end() || !found->second.has_snapshot) {
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
    Entry& entry = entries_[memory_key];
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
    Entry& entry = entries_[memory_key];
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
    for (auto& item : entries_) {
        Entry& entry = item.second;
        if (!entry.pending) {
            continue;
        }
        entry.pending = false;
        entry.result.status = MemoryReadyResult::Status::Cancelled;
        ++entry.result.snapshot_version;
    }
    cv_.notify_all();
}

} // namespace scheduler
