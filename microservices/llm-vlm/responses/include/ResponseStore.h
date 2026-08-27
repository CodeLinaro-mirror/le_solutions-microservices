// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using ResponseStoreJson = nlohmann::ordered_json;

/**
 * @brief Store lifecycle states for a Responses API turn.
 * @details There is no Deleted state; delete is represented by hard erase.
 */
enum class StoredResponseStatus {
    InProgress,
    Completed,
    Failed,
    Cancelled,
    Expired
};

/**
 * @brief Branch-local conversation memory captured after a completed turn.
 */
struct StoredConversationMemory {
    std::string summary_content;
    int summary_token_count = 0;
    std::unordered_map<std::string, std::string> facts;
    std::string summarized_until_response_id;
};

/**
 * @brief One stored Responses API turn.
 * @details The tree edge is previous_response_id. Context is derived by walking
 * completed ancestors, not from a session-level transcript.
 */
struct StoredResponse {
    std::string response_id;
    std::string session_id;
    std::string previous_response_id;
    std::string model;

    StoredResponseStatus status = StoredResponseStatus::InProgress;

    ResponseStoreJson input_items = ResponseStoreJson::array();
    ResponseStoreJson request_messages = ResponseStoreJson::array();
    ResponseStoreJson assistant_messages = ResponseStoreJson::array();

    ResponseStoreJson output_items = ResponseStoreJson::array();
    ResponseStoreJson response_object = ResponseStoreJson::object();
    ResponseStoreJson usage = nullptr;
    ResponseStoreJson error = nullptr;
    ResponseStoreJson incomplete_details = nullptr;
    ResponseStoreJson metadata = ResponseStoreJson::object();

    std::optional<StoredConversationMemory> conversation_memory;
    std::string active_job_id;

    int created_at = 0;
    int updated_at = 0;
    int completed_at = 0;
    int expires_at = 0;
};

/**
 * @brief Grouping container for a response tree.
 * @details The session tracks membership only; memory is branch-local to
 * individual responses.
 */
struct StoredSession {
    std::string session_id;
    std::string root_response_id;
    std::unordered_set<std::string> response_ids;
    int created_at = 0;
    int last_activity_at = 0;
};

/**
 * @brief Result of a read-only context walk.
 */
struct BuildCandidateResult {
    bool ok = false;
    int http_status = 200;
    std::string error_message;
    std::string session_id;
    ResponseStoreJson ancestor_messages = ResponseStoreJson::array();
    std::vector<std::string> ancestor_message_response_ids;
    ResponseStoreJson current_request_messages = ResponseStoreJson::array();
    std::optional<StoredConversationMemory> conversation_memory;
};

/**
 * @brief Result shape reserved for the future beginResponse mutation.
 */
struct BeginResponseResult {
    bool ok = false;
    int http_status = 200;
    std::string error_message;
    std::string response_id;
    std::string session_id;
    int created_at = 0;
    ResponseStoreJson ancestor_messages = ResponseStoreJson::array();
    std::vector<std::string> ancestor_message_response_ids;
    ResponseStoreJson current_request_messages = ResponseStoreJson::array();
    std::optional<StoredConversationMemory> conversation_memory;
};

/**
 * @brief Result status for the cancel mutation.
 */
enum class CancelOutcome {
    Cancelled,
    AlreadyCancelled,
    NotFound,
    InvalidState
};

/**
 * @brief Result of a client-initiated cancel transition.
 */
struct CancelResponseResult {
    bool ok = false;
    int http_status = 200;
    std::string error_message;
    std::string active_job_id;
    ResponseStoreJson response_object = ResponseStoreJson::object();
    CancelOutcome outcome = CancelOutcome::NotFound;
};

/**
 * @brief Job id returned by the future stale-in-progress expiry sweep.
 */
struct ExpiredResponse {
    std::string response_id;
    std::string active_job_id;
};

/**
 * @brief Result shape reserved for the future hard cascade delete mutation.
 */
struct DeleteCascadeResult {
    bool ok = false;
    int http_status = 200;
    std::string error_message;
};

class ResponseStoreTestAccess;

/**
 * @brief In-memory source of truth for Responses API response lineage.
 * @details Controller wiring lands in later ResponseStore phases.
 */
class ResponseStore {
public:
    /**
     * @brief Return the process-local singleton store.
     */
    static ResponseStore& getInstance();

    /**
     * @brief Create an InProgress response record and return its context walk.
     */
    BeginResponseResult beginResponse(
        const std::string& response_id,
        const std::string& model,
        const std::string& previous_response_id,
        const ResponseStoreJson& input_items,
        const ResponseStoreJson& request_messages,
        const ResponseStoreJson& metadata,
        int ttl_seconds = 900);

    /**
     * @brief Create an InProgress response using a prevalidated context walk.
     * @details The store still validates response identity and parent state
     * under lock before committing the new response.
     */
    BeginResponseResult beginResponseFromCandidate(
        const std::string& response_id,
        const std::string& model,
        const std::string& previous_response_id,
        const ResponseStoreJson& input_items,
        const ResponseStoreJson& request_messages,
        const ResponseStoreJson& metadata,
        BuildCandidateResult candidate,
        int ttl_seconds = 900);

    /**
     * @brief Commit an InProgress response as Completed.
     */
    bool completeResponse(
        const std::string& response_id,
        const ResponseStoreJson& assistant_messages,
        const ResponseStoreJson& output_items,
        const ResponseStoreJson& response_object,
        const ResponseStoreJson& usage,
        const std::optional<StoredConversationMemory>& memory_update =
            std::nullopt);

    /** @brief Idempotently attach asynchronously prepared conversation memory. */
    bool updateConversationMemory(
        const std::string& response_id,
        const StoredConversationMemory& memory_update);

    /**
     * @brief Mark an InProgress response as Failed.
     */
    bool failResponse(const std::string& response_id,
                      const ResponseStoreJson& error_object);

    /**
     * @brief Mark an InProgress response as Cancelled.
     */
    CancelResponseResult cancelResponseDetailed(
        const std::string& response_id);

    /**
     * @brief Hard-delete a response and descendants with two-pass validation.
     */
    DeleteCascadeResult deleteCascade(const std::string& response_id);

    /**
     * @brief Expire stale InProgress responses and return job ids to cancel.
     */
    std::vector<ExpiredResponse> expireStaleInProgress(int now_unix);

    /**
     * @brief Return a copy of a stored response, or nullopt if missing.
     */
    std::optional<StoredResponse> getResponse(
        const std::string& response_id) const;

    /**
     * @brief Return the active scheduler job id for an InProgress response.
     */
    std::optional<std::string> getActiveJob(
        const std::string& response_id) const;

    /**
     * @brief Return the normalized input items for a stored response.
     */
    std::optional<ResponseStoreJson> getInputItems(
        const std::string& response_id) const;

    /**
     * @brief Walk completed ancestors and keep current request messages separate.
     */
    BuildCandidateResult buildCandidateMessages(
        const std::string& previous_response_id,
        const ResponseStoreJson& request_messages) const;

    /**
     * @brief Clear all maps for standalone unit tests.
     */
    void clearForTest();

private:
    friend class ResponseStoreTestAccess;

    ResponseStore() = default;
    ResponseStore(const ResponseStore&) = delete;
    ResponseStore& operator=(const ResponseStore&) = delete;

    static int currentUnixTime();

    static void appendMessagesWithSource(
        ResponseStoreJson& destination,
        std::vector<std::string>& destination_response_ids,
        const ResponseStoreJson& messages,
        const std::string& response_id);

    static ResponseStoreJson makeResponseObject(
        const StoredResponse& response,
        const std::string& public_status,
        const ResponseStoreJson& output_items,
        const ResponseStoreJson& usage,
        const ResponseStoreJson& error,
        const ResponseStoreJson& incomplete_details);

    static BuildCandidateResult makeCandidateError(
        int http_status,
        const std::string& error_message,
        const ResponseStoreJson& request_messages);

    static BeginResponseResult makeBeginError(
        int http_status,
        const std::string& error_message);

    BuildCandidateResult buildCandidateMessagesLocked(
        const std::string& previous_response_id,
        const ResponseStoreJson& request_messages) const;

    BeginResponseResult beginResponseFromCandidateLocked(
        const std::string& response_id,
        const std::string& model,
        const std::string& previous_response_id,
        const ResponseStoreJson& input_items,
        const ResponseStoreJson& request_messages,
        const ResponseStoreJson& metadata,
        BuildCandidateResult candidate,
        int ttl_seconds);

    std::unordered_map<std::string, StoredResponse> responses_by_id_;
    std::unordered_map<std::string, StoredSession> sessions_by_id_;
    std::unordered_map<std::string, std::vector<std::string>> children_by_response_id_;
    mutable std::mutex mu_;
};
