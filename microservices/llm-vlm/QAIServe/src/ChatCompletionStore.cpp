// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ChatCompletionStore.h"
#include "ChatCompletionUtils.h"
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
    // Register continuation hash
    if (!session.continuation_hash.empty()) {
        hash_to_completion_id_[session.continuation_hash] = session.completion_id;
    }

    // Register retry candidate hash
    if (!session.retry_candidate_hash.empty()) {
        hash_to_completion_id_[session.retry_candidate_hash] = session.completion_id;
    }

    // Register tool call hash
    if (!session.tool_call_hash.empty()) {
        hash_to_completion_id_[session.tool_call_hash] = session.completion_id;
    }

    // Register job ID mapping
    if (!session.active_job_id.empty()) {
        job_to_completion_id_[session.active_job_id] = session.completion_id;
    }
}

void ChatCompletionStore::unregisterHashMappings(const ChatSession& session) {
    // Unregister continuation hash
    if (!session.continuation_hash.empty()) {
        hash_to_completion_id_.erase(session.continuation_hash);
    }

    // Unregister retry candidate hash
    if (!session.retry_candidate_hash.empty()) {
        hash_to_completion_id_.erase(session.retry_candidate_hash);
    }

    // Unregister tool call hash
    if (!session.tool_call_hash.empty()) {
        hash_to_completion_id_.erase(session.tool_call_hash);
    }

    // Unregister job ID mapping
    if (!session.active_job_id.empty()) {
        job_to_completion_id_.erase(session.active_job_id);
    }
}

std::pair<ChatSession*, bool> ChatCompletionStore::findOrCreateSession(
    const json& messages,
    const std::string& user_hint_id
) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!messages.is_array()) {
        LOG_ERROR << "findOrCreateSession: messages is not an array";
        return {nullptr, false};
    }

    // Step 1: Check user-provided hint ID
    if (!user_hint_id.empty()) {
        auto it = sessions_.find(user_hint_id);
        if (it != sessions_.end()) {
            it->second.last_accessed = std::chrono::steady_clock::now();
            LOG_DEBUG << "Found session by user hint ID: " << user_hint_id;
            return {&it->second, false};
        }
    }

    // Step 2: Check retry candidate hash (idempotent retry)
    // Hash of ALL complete pairs - matches if client resends exact same request
    std::string retry_hash = ChatCompletionUtils::hashConversationPairs(messages, false);
    if (!retry_hash.empty()) {
        auto hash_it = hash_to_completion_id_.find(retry_hash);
        if (hash_it != hash_to_completion_id_.end()) {
            auto& session = sessions_[hash_it->second];
            session.last_accessed = std::chrono::steady_clock::now();
            LOG_DEBUG << "Found session by retry candidate hash: " << retry_hash;
            return {&session, false};
        }
    }

    // Step 3: Check continuation hash (next turn)
    // Hash excluding last pair - matches if client adds a new user message
    std::string continuation_hash = ChatCompletionUtils::hashConversationPairs(messages, true);
    if (!continuation_hash.empty()) {
        auto hash_it = hash_to_completion_id_.find(continuation_hash);
        if (hash_it != hash_to_completion_id_.end()) {
            auto& session = sessions_[hash_it->second];
            session.last_accessed = std::chrono::steady_clock::now();
            LOG_DEBUG << "Found session by continuation hash: " << continuation_hash;
            return {&session, false};
        }
    }

    // Step 4: Check tool call hash (tool response submission)
    // Find last assistant message with tool_calls
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if ((*it).is_object() &&
            ChatCompletionUtils::safeGet<std::string>(*it, "role", "") == "assistant" &&
            ChatCompletionUtils::hasToolCalls(*it)) {

            // Create array of messages up to and including this assistant message
            json messages_up_to_tool_call = json::array();
            for (auto msg_it = messages.begin(); msg_it != it.base(); ++msg_it) {
                messages_up_to_tool_call.push_back(*msg_it);
            }

            std::string tool_hash = ChatCompletionUtils::hashSpecificMessages(messages_up_to_tool_call);
            if (!tool_hash.empty()) {
                auto tool_it = hash_to_completion_id_.find(tool_hash);
                if (tool_it != hash_to_completion_id_.end()) {
                    auto& session = sessions_[tool_it->second];
                    session.last_accessed = std::chrono::steady_clock::now();
                    LOG_DEBUG << "Found session by tool call hash: " << tool_hash;
                    return {&session, false};
                }
            }
            break;
        }
    }

    // Step 5: Create new session
    std::string new_id = generateUUID();
    ChatSession& session = sessions_[new_id];
    session.completion_id = new_id;
    session.messages = json::array();  // Start with empty history - messages will be added after first turn
    session.continuation_hash = continuation_hash;
    session.retry_candidate_hash = retry_hash;
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
    return {&session, true};
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

    // Unregister old hash mappings
    unregisterHashMappings(session);

    // Update messages
    session.messages = messages;
    session.last_accessed = std::chrono::steady_clock::now();

    // Recalculate hashes
    // continuation_hash = hash of ALL complete pairs (for next turn lookup)
    // retry_candidate_hash = hash excluding last pair (for idempotent retry lookup)
    session.continuation_hash = ChatCompletionUtils::hashConversationPairs(messages, false);
    session.retry_candidate_hash = ChatCompletionUtils::hashConversationPairs(messages, true);

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
