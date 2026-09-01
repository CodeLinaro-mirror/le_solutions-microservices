// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ChatCompletionStore.h"
#include "ChatCompletionUtils.h"
#include <qai_forge/QaiForge.h>
#include <drogon/drogon.h>
#include <random>
#include <sstream>
#include <iomanip>

std::string ChatCompletionStore::generateUUID() {
    // Generate a simple UUID v4
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    std::uniform_int_distribution<> dis2(8, 11);

    std::stringstream ss;
    ss << std::hex;

    for (int i = 0; i < 8; i++) {
        ss << dis(gen);
    }
    ss << "-";

    for (int i = 0; i < 4; i++) {
        ss << dis(gen);
    }
    ss << "-4"; // Version 4

    for (int i = 0; i < 3; i++) {
        ss << dis(gen);
    }
    ss << "-";

    ss << dis2(gen); // Variant

    for (int i = 0; i < 3; i++) {
        ss << dis(gen);
    }
    ss << "-";

    for (int i = 0; i < 12; i++) {
        ss << dis(gen);
    }

    return ss.str();
}

void ChatCompletionStore::registerHashMappings(ChatSession& session) {
    if (!session.continuation_hash.empty()) {
        hash_to_completion_id_[session.continuation_hash] = session.completion_id;
    }
    if (!session.retry_candidate_hash.empty()) {
        hash_to_completion_id_[session.retry_candidate_hash] = session.completion_id;
    }
    if (!session.tool_call_hash.empty()) {
        hash_to_completion_id_[session.tool_call_hash] = session.completion_id;
    }
    if (!session.active_job_id.empty()) {
        job_to_completion_id_[session.active_job_id] = session.completion_id;
    }
}

void ChatCompletionStore::unregisterHashMappings(const ChatSession& session) {
    if (!session.continuation_hash.empty()) {
        hash_to_completion_id_.erase(session.continuation_hash);
    }
    if (!session.retry_candidate_hash.empty()) {
        hash_to_completion_id_.erase(session.retry_candidate_hash);
    }
    if (!session.tool_call_hash.empty()) {
        hash_to_completion_id_.erase(session.tool_call_hash);
    }
    if (!session.active_job_id.empty()) {
        job_to_completion_id_.erase(session.active_job_id);
    }
}

SessionLookupResult ChatCompletionStore::findOrCreateSession(
    const json& messages,
    const json& request_body,
    const std::string& user_hint_id
) {
    std::lock_guard<std::mutex> lock(mutex_);

    SessionLookupResult result;

    if (!messages.is_array()) {
        LOG_ERROR << "findOrCreateSession: messages is not an array";
        return result;
    }

    // Step 1: Check user-provided hint ID
    if (!user_hint_id.empty()) {
        auto it = sessions_.find(user_hint_id);
        if (it != sessions_.end()) {
            it->second.last_accessed = std::chrono::steady_clock::now();
            LOG_DEBUG << "Found session by user hint ID: " << user_hint_id;
            result.session = &it->second;
            result.is_new = false;
            return result;
        }
    }

    // Step 2: Check tool-call hash (in-progress tool-calling round-trip).
    // Checked BEFORE the rotating continuation/retry hash — an in-progress
    // tool-calling sequence (assistant tool_calls with no final answer yet)
    // is excluded from hashConversationPairs() entirely (see
    // ChatCompletionUtils::identifyCompletePairs), so it must not be
    // misrouted to an unrelated session via the general-purpose hash below.
    // Hash is computed over ONLY the user messages in the trailing
    // tool-calling sequence — stable regardless of assistant formatting,
    // matching the legacy Python calculate_hash_for_specific_messages(user_messages)
    // approach, rather than "all messages up to and including the tool-call
    // assistant message".
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if ((*it).is_object() &&
            ChatCompletionUtils::safeGet<std::string>(*it, "role", "") == "assistant" &&
            ChatCompletionUtils::hasToolCalls(*it)) {

            json user_messages_only = json::array();
            for (auto msg_it = messages.begin(); msg_it != it.base(); ++msg_it) {
                if ((*msg_it).is_object() &&
                    ChatCompletionUtils::safeGet<std::string>(*msg_it, "role", "") == "user") {
                    user_messages_only.push_back(*msg_it);
                }
            }

            std::string tool_hash = ChatCompletionUtils::hashSpecificMessages(user_messages_only);
            if (!tool_hash.empty()) {
                auto tool_it = hash_to_completion_id_.find(tool_hash);
                if (tool_it != hash_to_completion_id_.end()) {
                    auto session_it = sessions_.find(tool_it->second);
                    if (session_it != sessions_.end()) {
                        session_it->second.last_accessed = std::chrono::steady_clock::now();
                        LOG_DEBUG << "Found session by tool call hash: " << tool_hash;
                        result.session = &session_it->second;
                        result.is_new = false;
                        return result;
                    }
                }
            }
            break;
        }
    }

    // Step 3: Rotating hash — direct port of the legacy Python
    // SessionManager.find_or_create_session() rotating continuation_hash /
    // retry_candidate_hash design. Compute ONE hash from the incoming
    // request (of ALL complete pairs currently present).
    std::string full_hash = ChatCompletionUtils::hashConversationPairs(messages, false);
    if (!full_hash.empty()) {
        auto hash_it = hash_to_completion_id_.find(full_hash);
        if (hash_it != hash_to_completion_id_.end()) {
            auto session_it = sessions_.find(hash_it->second);
            if (session_it != sessions_.end()) {
                ChatSession& session = session_it->second;

                if (full_hash == session.continuation_hash) {
                    // Next-turn continuation match.
                    session.last_accessed = std::chrono::steady_clock::now();
                    LOG_DEBUG << "Found session by continuation hash: " << full_hash;
                    result.session = &session;
                    result.is_new = false;
                    return result;
                }

                if (full_hash == session.retry_candidate_hash) {
                    // Hash match on the retry candidate alone is not
                    // sufficient proof of "same request" — verify the full
                    // request signature before replaying a cached response.
                    // Deliberately does NOT fall back to reusing the session
                    // on a signature mismatch (or missing cached replay) —
                    // falls through to create a brand-new session instead,
                    // matching the legacy Python "don't guess" behavior.
                    if (!request_body.is_null() &&
                        !session.last_request_signature.empty() &&
                        !session.last_replay_result.is_null()) {
                        std::string incoming_signature =
                            ChatCompletionUtils::buildRequestSignature(request_body, messages);
                        if (incoming_signature == session.last_request_signature) {
                            session.last_accessed = std::chrono::steady_clock::now();
                            LOG_DEBUG << "Found retry replay for session "
                                      << session.completion_id
                                      << " by hash " << full_hash;
                            result.session = &session;
                            result.is_new = false;
                            result.replay_result = session.last_replay_result;
                            return result;
                        }
                    }
                    // Signature mismatch (or no cached replay available) —
                    // fall through to create a new session below.
                }
            }
        }
    }

    // Step 4: Create new session
    std::string new_id = generateUUID();
    ChatSession& session = sessions_[new_id];
    session.completion_id = new_id;
    session.messages = json::array();
    session.continuation_hash = full_hash;
    session.created_at = std::chrono::steady_clock::now();
    session.last_accessed = session.created_at;

    // Extract model from messages if present (look for system message or use default)
    for (const auto& msg : messages) {
        if (msg.is_object() && msg.contains("model")) {
            session.model = msg["model"].get<std::string>();
            break;
        }
    }

    // Register hash mappings
    registerHashMappings(session);

    LOG_INFO << "Created new session: " << new_id;
    result.session = &session;
    result.is_new = true;
    return result;
}

ChatSession* ChatCompletionStore::findByCompletionId(const std::string& completion_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = sessions_.find(completion_id);
    if (it != sessions_.end()) {
        it->second.last_accessed = std::chrono::steady_clock::now();
        return &it->second;
    }

    return nullptr;
}

ChatSession* ChatCompletionStore::findByJobId(const std::string& job_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = job_to_completion_id_.find(job_id);
    if (it != job_to_completion_id_.end()) {
        auto session_it = sessions_.find(it->second);
        if (session_it != sessions_.end()) {
            session_it->second.last_accessed = std::chrono::steady_clock::now();
            return &session_it->second;
        }
    }

    return nullptr;
}

void ChatCompletionStore::updateSession(const std::string& completion_id, const json& messages) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = sessions_.find(completion_id);
    if (it == sessions_.end()) {
        LOG_WARN << "updateSession: session not found: " << completion_id;
        return;
    }

    ChatSession& session = it->second;

    // Unregister old hash mappings (continuation/retry/tool-call/job-id)
    unregisterHashMappings(session);

    // Update messages
    session.messages = messages;
    session.last_accessed = std::chrono::steady_clock::now();

    // Rotate the hash pair — direct port of the legacy Python
    // ConversationSession.complete_current_event() rotation:
    //   retry_candidate_hash = (old) continuation_hash
    //   continuation_hash    = hash of ALL complete pairs as of THIS turn
    // A freshly computed hash can legitimately come back empty (e.g. no
    // complete pairs yet) — in that case, do not overwrite continuation_hash,
    // since that just means there is nothing new to key on, not that the
    // previously-registered hash should be discarded.
    std::string new_hash = ChatCompletionUtils::hashConversationPairs(messages, false);
    session.retry_candidate_hash = session.continuation_hash;
    if (!new_hash.empty()) {
        session.continuation_hash = new_hash;
    }

    // Register new hash mappings
    registerHashMappings(session);

    LOG_DEBUG << "Updated session: " << completion_id;
}

void ChatCompletionStore::deleteSession(const std::string& completion_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = sessions_.find(completion_id);
    if (it == sessions_.end()) {
        LOG_WARN << "deleteSession: session not found: " << completion_id;
        return;
    }

    // Release any per-session backend state (e.g. LiteRT-LM KV cache
    // session) scoped to the model this session actually used. Centralized
    // here — at the store layer — rather than in individual controllers, so
    // every current and future caller of deleteSession() (any transport)
    // gets this cleanup automatically without needing to remember to add it.
    if (!it->second.model.empty()) {
        qai_forge::QaiForge::getInstance().clearSession(it->second.model, completion_id);
    }

    // Unregister hash mappings
    unregisterHashMappings(it->second);

    // Remove session
    sessions_.erase(it);

    LOG_INFO << "Deleted session: " << completion_id;
}

size_t ChatCompletionStore::cleanupExpiredSessions(std::chrono::seconds max_age) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::steady_clock::now();
    std::vector<std::string> expired_ids;

    // Find expired sessions
    for (const auto& [id, session] : sessions_) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - session.last_accessed);
        if (age > max_age) {
            expired_ids.push_back(id);
        }
    }

    // Delete expired sessions
    for (const auto& id : expired_ids) {
        auto it = sessions_.find(id);
        if (it != sessions_.end()) {
            if (!it->second.model.empty()) {
                qai_forge::QaiForge::getInstance().clearSession(it->second.model, id);
            }
            unregisterHashMappings(it->second);
            sessions_.erase(it);
        }
    }

    if (!expired_ids.empty()) {
        LOG_INFO << "Cleaned up " << expired_ids.size() << " expired sessions";
    }

    return expired_ids.size();
}

size_t ChatCompletionStore::getSessionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

std::vector<std::string> ChatCompletionStore::getAllSessionIds() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> ids;
    ids.reserve(sessions_.size());

    for (const auto& [id, _] : sessions_) {
        ids.push_back(id);
    }

    return ids;
}
