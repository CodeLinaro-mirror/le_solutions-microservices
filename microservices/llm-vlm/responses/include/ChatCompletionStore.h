// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <chrono>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::ordered_json;

/**
 * @brief Completed and in-flight state for one Chat Completions conversation.
 */
struct ChatSession {
    std::string completion_id;
    std::string model;
    json messages = json::array();

    std::string memory_turn_id;
    std::string retry_parent_turn_id;
    std::string active_turn_id;
    std::string active_parent_turn_id;
    json active_request_messages = json::array();

    std::string continuation_hash;
    std::string retry_candidate_hash;
    std::string tool_call_hash;

    std::string last_request_signature;
    json last_replay_result;

    bool has_active_tool_call = false;
    std::chrono::steady_clock::time_point tool_call_timestamp;
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point last_accessed;
};

/**
 * @brief Value returned when a request atomically starts a Chat turn.
 */
struct BeginChatTurnResult {
    bool ok = false;
    bool is_new = false;
    std::string error_message;
    std::string completion_id;
    std::string model;
    std::string turn_id;
    std::string parent_turn_id;
    std::optional<json> replay_result;
};

/**
 * @brief Thread-safe store for Chat Completions transcript and turn lineage.
 */
class ChatCompletionStore {
public:
    ChatCompletionStore() = default;
    ~ChatCompletionStore() = default;

    ChatCompletionStore(const ChatCompletionStore&) = delete;
    ChatCompletionStore& operator=(const ChatCompletionStore&) = delete;

    /**
     * @brief Resolve a session and reserve one unique in-flight turn.
     * @param messages Complete client-supplied transcript for this request.
     * @param request_body Full request body used to validate idempotent retries.
     * @param model Requested model identifier.
     * @param user_hint_id Optional client-provided session identifier.
     * @return A value snapshot containing the session and memory parent IDs.
     */
    BeginChatTurnResult beginTurn(
        const json& messages,
        const json& request_body,
        const std::string& model,
        const std::string& user_hint_id = "");

    /**
     * @brief Commit the assistant message if the expected turn is still active.
     */
    bool completeTurn(const std::string& completion_id,
                      const std::string& turn_id,
                      const json& assistant_message,
                      const json& request_body = json(),
                      const json& replay_result = json());

    /**
     * @brief Clear an in-flight turn after rejection, failure, or cancellation.
     */
    bool abortTurn(const std::string& completion_id,
                   const std::string& turn_id);

    /**
     * @brief Return a copy of a session, or nullopt when it is absent.
     */
    std::optional<ChatSession> getSession(
        const std::string& completion_id) const;

    /**
     * @brief Delete a session and all of its hash mappings.
     * @return true when the session existed.
     */
    bool deleteSession(const std::string& completion_id);

    /**
     * @brief Remove sessions older than max_age.
     * @return Completion IDs removed by the sweep.
     */
    std::vector<std::string> cleanupExpiredSessions(
        std::chrono::seconds max_age);

    size_t getSessionCount() const;
    std::vector<std::string> getAllSessionIds() const;

private:
    enum class MatchKind {
        NewSession,
        Hint,
        Retry,
        Continuation,
        ToolContinuation,
    };

    struct SessionMatch {
        std::string completion_id;
        std::string parent_turn_id;
        MatchKind kind = MatchKind::NewSession;
        std::optional<json> replay_result;
    };

    std::string generateUUID();
    SessionMatch findSessionLocked(const json& messages,
                                   const json& request_body,
                                   const std::string& user_hint_id) const;
    void registerHashMappings(ChatSession& session);
    void unregisterHashMappings(const ChatSession& session);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ChatSession> sessions_;
    std::unordered_map<std::string, std::string> continuation_index_;
    std::unordered_map<std::string, std::string> retry_index_;
    std::unordered_map<std::string, std::string> tool_call_index_;
};
