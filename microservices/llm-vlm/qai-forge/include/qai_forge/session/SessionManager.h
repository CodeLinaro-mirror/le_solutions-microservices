// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/session/ConversationSession.h"
#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <memory>
#include <optional>
#include <functional>

// ─────────────────────────────────────────────────────────────────────────────
// SessionManager — Layer 2 Singleton (Section 3.D of architecture design)
//
// Manages the lifecycle of ConversationSession objects.
// Provides hash-based session lookup for stable chat completion IDs.
//
// Design decisions:
//   - Thread-safe via shared_mutex (multiple readers, single writer).
//   - Sessions are keyed by session_id (the stable chat completion ID).
//   - Hash-based lookup allows finding sessions by conversation content hash
//     (for retry/replay detection).
//   - Does NOT contain inference logic or HTTP concepts.
// ─────────────────────────────────────────────────────────────────────────────
class SessionManager {
public:
    static SessionManager& getInstance();

    // ── Session lifecycle ──────────────────────────────────────────────────────

    /**
     * Find an existing session by ID, or create a new one.
     * Returns the session and whether it was newly created.
     */
    std::pair<std::shared_ptr<ConversationSession>, bool>
    findOrCreate(const std::string& session_id, const std::string& user_id = "default_user");

    /**
     * Get an existing session by ID. Returns nullptr if not found.
     */
    std::shared_ptr<ConversationSession> getSession(const std::string& session_id) const;

    /**
     * Delete a session and all associated resources.
     * Returns true if the session was found and deleted.
     */
    bool deleteSession(const std::string& session_id);

    /**
     * Generate a new unique session ID.
     */
    static std::string generateSessionId();

    /**
     * Calculate a stable hash for a set of messages.
     * Used for retry/replay detection.
     */
    static std::string calculateMessagesHash(const json& messages);

    /**
     * Find a session by conversation content hash.
     * Returns nullptr if no matching session found.
     */
    std::shared_ptr<ConversationSession> findByHash(const std::string& hash) const;

    /**
     * Register a hash → session_id mapping (for retry detection).
     */
    void registerHash(const std::string& hash, const std::string& session_id);

    /**
     * Unregister a hash mapping.
     */
    void unregisterHash(const std::string& hash);

    /**
     * Get total number of active sessions.
     */
    size_t sessionCount() const;

private:
    SessionManager() = default;
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<ConversationSession>> sessions_;
    std::unordered_map<std::string, std::string> hash_to_session_id_;
};
