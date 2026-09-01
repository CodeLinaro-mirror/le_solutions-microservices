// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <nlohmann/json.hpp>

using json = nlohmann::ordered_json;

/**
 * @brief Represents a chat completion session
 *
 * Stores conversation history and hash-based lookup keys for stateless
 * session identification (port of Python SessionManager).
 */
struct ChatSession {
    std::string completion_id;           // Unique session ID (UUID)
    std::string model;                   // Model name — always kept in sync
                                          // with the most recently used model
                                          // on this session (see updateSession)
    json messages;                       // Full conversation history

    std::string active_job_id;           // Current qai-forge job ID (if any)

    // ── Rotating hash pair for session resolution ────────────────────────────
    // Direct port of Python's ConversationSession.continuation_hash /
    // retry_candidate_hash. Updated by rotation on every completed turn:
    //   retry_candidate_hash = (old) continuation_hash
    //   continuation_hash    = hash of ALL complete pairs as of THIS turn
    // continuation_hash matches the NEXT request (this history + one new
    // pending user message). retry_candidate_hash matches an exact retry of
    // the request that produced the CURRENT continuation_hash.
    std::string continuation_hash;
    std::string retry_candidate_hash;

    std::string tool_call_hash;          // For tool response submission

    // ── Idempotent-retry replay cache ─────────────────────────────────────────
    // Direct port of Python's ConversationEvent.request_signature /
    // replay_result. A retry_candidate_hash match alone is not sufficient
    // proof of "same request" — last_request_signature must ALSO match a
    // freshly computed signature of the incoming request before
    // last_replay_result is served back to the client without re-running
    // inference. See ChatCompletionUtils::buildRequestSignature().
    std::string last_request_signature;
    json last_replay_result;             // {"content", "finish_reason", "tool_calls"?}

    // Tool calling state
    bool has_active_tool_call = false;
    std::chrono::steady_clock::time_point tool_call_timestamp;

    // Metadata
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point last_accessed;
};

/**
 * @brief Result of a session lookup/creation attempt.
 *
 * When replay_result has a value, the caller MUST serve that cached response
 * directly (no inference call) — this represents a signature-verified
 * idempotent retry of the request that produced the session's current
 * continuation_hash. session/is_new are still populated in this case for
 * logging/consistency, but the caller should not treat this as a "new turn".
 */
struct SessionLookupResult {
    ChatSession* session = nullptr;
    bool is_new = false;
    std::optional<json> replay_result;
};

/**
 * @brief Thread-safe store for chat completion sessions
 *
 * Port of Python SessionManager with hash-based session lookup.
 * Provides stateless session identification using SHA-256 hashes
 * of conversation content.
 */
class ChatCompletionStore {
public:
    ChatCompletionStore() = default;
    ~ChatCompletionStore() = default;

    // Prevent copying
    ChatCompletionStore(const ChatCompletionStore&) = delete;
    ChatCompletionStore& operator=(const ChatCompletionStore&) = delete;

    /**
     * @brief Find or create a session based on messages
     *
     * Direct port of Python's SessionManager.find_or_create_session().
     * Lookup order:
     *   1. User-provided hint ID
     *   2. Tool-call hash (in-progress tool-calling round-trip) — checked
     *      BEFORE the rotating continuation/retry hash, since an in-progress
     *      tool-calling sequence is excluded from the completed-pairs hash
     *      entirely and must not be misrouted to an unrelated session.
     *   3. Rotating hash: match against continuation_hash (next-turn) or,
     *      if the request signature also matches, retry_candidate_hash
     *      (idempotent retry — returns a cached replay_result).
     *   4. Create new session if no match found.
     *
     * @param messages Array of message objects
     * @param request_body Full parsed request body (for signature verification
     *                     on retry_candidate_hash match). May be empty/null if
     *                     the caller does not have it (disables replay).
     * @param user_hint_id Optional user-provided session ID
     * @return SessionLookupResult — see struct docs for replay_result semantics
     */
    SessionLookupResult findOrCreateSession(
        const json& messages,
        const json& request_body = json(),
        const std::string& user_hint_id = ""
    );

    /**
     * @brief Find session by completion ID
     *
     * @param completion_id Session ID
     * @return Session pointer or nullptr if not found
     */
    ChatSession* findByCompletionId(const std::string& completion_id);

    /**
     * @brief Find session by active job ID
     *
     * @param job_id qai-forge job ID
     * @return Session pointer or nullptr if not found
     */
    ChatSession* findByJobId(const std::string& job_id);

    /**
     * @brief Update session with new messages
     *
     * Updates the session's message history and rotates the continuation/
     * retry-candidate hash pair (see ChatSession field docs).
     *
     * @param completion_id Session ID
     * @param messages New message array
     */
    void updateSession(const std::string& completion_id, const json& messages);

    /**
     * @brief Delete a session
     *
     * Also releases any per-session backend state (e.g. LiteRT-LM KV cache
     * session) via QaiForge::clearSession(), scoped to the session's model.
     *
     * @param completion_id Session ID
     */
    void deleteSession(const std::string& completion_id);

    /**
     * @brief Clean up expired sessions
     *
     * Removes sessions that haven't been accessed within max_age.
     *
     * @param max_age Maximum age for sessions
     * @return Number of sessions deleted
     */
    size_t cleanupExpiredSessions(std::chrono::seconds max_age);

    /**
     * @brief Get total number of active sessions
     *
     * @return Session count
     */
    size_t getSessionCount() const;

    /**
     * @brief Get all session IDs
     *
     * @return Vector of completion IDs
     */
    std::vector<std::string> getAllSessionIds() const;

private:
    /**
     * @brief Generate a new UUID for session ID
     *
     * @return UUID string
     */
    std::string generateUUID();

    /**
     * @brief Register hash mappings for a session
     *
     * @param session Session to register
     */
    void registerHashMappings(ChatSession& session);

    /**
     * @brief Unregister hash mappings for a session
     *
     * @param session Session to unregister
     */
    void unregisterHashMappings(const ChatSession& session);

    // Thread safety
    mutable std::mutex mutex_;

    // Primary storage
    std::unordered_map<std::string, ChatSession> sessions_;

    // Hash-based lookup indices
    std::unordered_map<std::string, std::string> hash_to_completion_id_;

    // Job ID lookup index
    std::unordered_map<std::string, std::string> job_to_completion_id_;
};
