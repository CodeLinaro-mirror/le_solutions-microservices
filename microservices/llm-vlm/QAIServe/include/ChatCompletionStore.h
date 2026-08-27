// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

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
    std::string model;                   // Model name
    json messages;                       // Full conversation history

    std::string active_job_id;           // Current qai-forge job ID (if any)

    // Hash-based lookup keys (for stateless session identification)
    std::string continuation_hash;       // For next-turn lookup
    std::string retry_candidate_hash;    // For idempotent retry
    std::string tool_call_hash;          // For tool response submission

    // Tool calling state
    bool has_active_tool_call = false;
    std::chrono::steady_clock::time_point tool_call_timestamp;

    // Metadata
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point last_accessed;
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
     * Port of Python's find_or_create_session() method.
     * Implements 5-step lookup algorithm:
     * 1. Check user-provided hint ID
     * 2. Check retry candidate hash (idempotent retry)
     * 3. Check continuation hash (next turn)
     * 4. Check tool call hash (tool response submission)
     * 5. Create new session if no match found
     *
     * @param messages Array of message objects
     * @param user_hint_id Optional user-provided session ID
     * @return Pair of (session pointer, is_new flag)
     */
    std::pair<ChatSession*, bool> findOrCreateSession(
        const json& messages,
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
     * Updates the session's message history and recalculates hashes.
     *
     * @param completion_id Session ID
     * @param messages New message array
     */
    void updateSession(const std::string& completion_id, const json& messages);

    /**
     * @brief Delete a session
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
