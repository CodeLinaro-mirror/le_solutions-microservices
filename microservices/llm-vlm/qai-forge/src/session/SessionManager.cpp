// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/session/SessionManager.h"
#include <sstream>
#include <iomanip>
#include <random>
#include <functional>
#include <mutex>

// ─────────────────────────────────────────────────────────────────────────────
// Singleton
// ─────────────────────────────────────────────────────────────────────────────
SessionManager& SessionManager::getInstance() {
    static SessionManager instance;
    return instance;
}

// ─────────────────────────────────────────────────────────────────────────────
// generateSessionId — Produces a stable "chatcmpl-<uuid>" style ID
// ─────────────────────────────────────────────────────────────────────────────
std::string SessionManager::generateSessionId() {
    static std::mt19937_64 rng(std::random_device{}());
    static std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss;
    oss << "chatcmpl-" << std::hex << std::setw(16) << std::setfill('0') << dist(rng);
    return oss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// calculateMessagesHash — Stable hash of message content for retry detection
// ─────────────────────────────────────────────────────────────────────────────
std::string SessionManager::calculateMessagesHash(const json& messages) {
    // Use a simple FNV-1a hash over the serialized messages string
    std::string serialized = messages.dump();
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : serialized) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << hash;
    return oss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// findOrCreate
// ─────────────────────────────────────────────────────────────────────────────
std::pair<std::shared_ptr<ConversationSession>, bool>
SessionManager::findOrCreate(const std::string& session_id, const std::string& user_id) {
    // Fast path: check with shared lock
    {
        std::shared_lock lock(mutex_);
        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            return {it->second, false};
        }
    }

    // Slow path: create with exclusive lock
    std::unique_lock lock(mutex_);
    // Double-check after acquiring exclusive lock
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        return {it->second, false};
    }

    auto session = std::make_shared<ConversationSession>(session_id, user_id);
    sessions_[session_id] = session;
    return {session, true};
}

// ─────────────────────────────────────────────────────────────────────────────
// getSession
// ─────────────────────────────────────────────────────────────────────────────
std::shared_ptr<ConversationSession> SessionManager::getSession(const std::string& session_id) const {
    std::shared_lock lock(mutex_);
    auto it = sessions_.find(session_id);
    return (it != sessions_.end()) ? it->second : nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// deleteSession
// ─────────────────────────────────────────────────────────────────────────────
bool SessionManager::deleteSession(const std::string& session_id) {
    std::unique_lock lock(mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return false;

    // Remove any hash mappings pointing to this session
    for (auto hash_it = hash_to_session_id_.begin(); hash_it != hash_to_session_id_.end(); ) {
        if (hash_it->second == session_id) {
            hash_it = hash_to_session_id_.erase(hash_it);
        } else {
            ++hash_it;
        }
    }

    sessions_.erase(it);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// findByHash
// ─────────────────────────────────────────────────────────────────────────────
std::shared_ptr<ConversationSession> SessionManager::findByHash(const std::string& hash) const {
    std::shared_lock lock(mutex_);
    auto hash_it = hash_to_session_id_.find(hash);
    if (hash_it == hash_to_session_id_.end()) return nullptr;

    auto session_it = sessions_.find(hash_it->second);
    return (session_it != sessions_.end()) ? session_it->second : nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// registerHash / unregisterHash
// ─────────────────────────────────────────────────────────────────────────────
void SessionManager::registerHash(const std::string& hash, const std::string& session_id) {
    std::unique_lock lock(mutex_);
    hash_to_session_id_[hash] = session_id;
}

void SessionManager::unregisterHash(const std::string& hash) {
    std::unique_lock lock(mutex_);
    hash_to_session_id_.erase(hash);
}

// ─────────────────────────────────────────────────────────────────────────────
// sessionCount
// ─────────────────────────────────────────────────────────────────────────────
size_t SessionManager::sessionCount() const {
    std::shared_lock lock(mutex_);
    return sessions_.size();
}
