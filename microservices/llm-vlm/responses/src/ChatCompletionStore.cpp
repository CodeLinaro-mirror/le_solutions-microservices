// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ChatCompletionStore.h"
#include "ChatCompletionUtils.h"

#include <qai_forge/QaiForge.h>

#include <drogon/drogon.h>
#include <iomanip>
#include <random>
#include <sstream>
#include <utility>

std::string ChatCompletionStore::generateUUID() {
    std::random_device random_device;
    std::mt19937 generator(random_device());
    std::uniform_int_distribution<> hex_digit(0, 15);
    std::uniform_int_distribution<> variant(8, 11);

    std::stringstream value;
    value << std::hex;
    for (int index = 0; index < 8; ++index) {
        value << hex_digit(generator);
    }
    value << "-";
    for (int index = 0; index < 4; ++index) {
        value << hex_digit(generator);
    }
    value << "-4";
    for (int index = 0; index < 3; ++index) {
        value << hex_digit(generator);
    }
    value << "-" << variant(generator);
    for (int index = 0; index < 3; ++index) {
        value << hex_digit(generator);
    }
    value << "-";
    for (int index = 0; index < 12; ++index) {
        value << hex_digit(generator);
    }
    return value.str();
}

ChatCompletionStore::SessionMatch ChatCompletionStore::findSessionLocked(
    const json& messages,
    const json& request_body,
    const std::string& user_hint_id) const {
    SessionMatch match;

    if (!user_hint_id.empty()) {
        auto session_it = sessions_.find(user_hint_id);
        if (session_it != sessions_.end()) {
            match.completion_id = user_hint_id;
            match.parent_turn_id = session_it->second.memory_turn_id;
            match.kind = MatchKind::Hint;
            return match;
        }
    }

    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (!it->is_object()
            || ChatCompletionUtils::safeGet<std::string>(
                *it, "role", "") != "assistant"
            || !ChatCompletionUtils::hasToolCalls(*it)) {
            continue;
        }

        json messages_through_tool_call = json::array();
        for (auto message_it = messages.begin();
             message_it != it.base();
             ++message_it) {
            messages_through_tool_call.push_back(*message_it);
        }
        const std::string tool_hash =
            ChatCompletionUtils::hashSpecificMessages(
                messages_through_tool_call);
        auto tool_it = tool_call_index_.find(tool_hash);
        if (!tool_hash.empty() && tool_it != tool_call_index_.end()) {
            auto session_it = sessions_.find(tool_it->second);
            if (session_it != sessions_.end()) {
                match.completion_id = session_it->first;
                match.parent_turn_id = session_it->second.memory_turn_id;
                match.kind = MatchKind::ToolContinuation;
                return match;
            }
        }
        break;
    }

    const std::string conversation_hash =
        ChatCompletionUtils::hashConversationPairs(messages, false);
    auto continuation_it = continuation_index_.find(conversation_hash);
    if (!conversation_hash.empty()
        && continuation_it != continuation_index_.end()) {
        auto session_it = sessions_.find(continuation_it->second);
        if (session_it != sessions_.end()) {
            match.completion_id = session_it->first;
            match.parent_turn_id = session_it->second.memory_turn_id;
            match.kind = MatchKind::Continuation;
            return match;
        }
    }

    auto retry_it = retry_index_.find(conversation_hash);
    if (!conversation_hash.empty() && retry_it != retry_index_.end()) {
        auto session_it = sessions_.find(retry_it->second);
        if (session_it != sessions_.end()
            && !session_it->second.last_request_signature.empty()
            && !session_it->second.last_replay_result.is_null()
            && ChatCompletionUtils::buildRequestSignature(
                   request_body, messages)
                == session_it->second.last_request_signature) {
            match.completion_id = session_it->first;
            match.kind = MatchKind::Retry;
            match.replay_result = session_it->second.last_replay_result;
            return match;
        }
    }

    return match;
}

BeginChatTurnResult ChatCompletionStore::beginTurn(
    const json& messages,
    const json& request_body,
    const std::string& model,
    const std::string& user_hint_id) {
    BeginChatTurnResult result;
    if (!messages.is_array()) {
        result.error_message = "messages is not an array";
        return result;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    SessionMatch match = findSessionLocked(
        messages, request_body, user_hint_id);
    ChatSession* session = nullptr;
    if (match.completion_id.empty()) {
        const std::string completion_id = generateUUID();
        ChatSession& created = sessions_[completion_id];
        created.completion_id = completion_id;
        created.continuation_hash =
            ChatCompletionUtils::hashConversationPairs(messages, false);
        created.created_at = std::chrono::steady_clock::now();
        created.last_accessed = created.created_at;
        registerHashMappings(created);
        session = &created;
        result.is_new = true;
    } else {
        session = &sessions_.at(match.completion_id);
    }

    if (!session->active_turn_id.empty()) {
        result.error_message =
            "Another request is already active for this chat session";
        return result;
    }

    session->model = model;
    if (match.replay_result.has_value()) {
        session->last_accessed = std::chrono::steady_clock::now();
        result.ok = true;
        result.completion_id = session->completion_id;
        result.model = session->model;
        result.replay_result = std::move(match.replay_result);
        return result;
    }

    session->active_turn_id = "chatturn_" + generateUUID();
    session->active_parent_turn_id = match.parent_turn_id;
    session->active_request_messages = messages;
    session->last_accessed = std::chrono::steady_clock::now();

    result.ok = true;
    result.completion_id = session->completion_id;
    result.model = session->model;
    result.turn_id = session->active_turn_id;
    result.parent_turn_id = session->active_parent_turn_id;
    return result;
}

bool ChatCompletionStore::completeTurn(
    const std::string& completion_id,
    const std::string& turn_id,
    const json& assistant_message,
    const json& request_body,
    const json& replay_result) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = sessions_.find(completion_id);
    if (found == sessions_.end()
        || found->second.active_turn_id != turn_id) {
        return false;
    }

    ChatSession& session = found->second;
    unregisterHashMappings(session);

    json request_messages = session.active_request_messages;
    json completed_messages = request_messages;
    completed_messages.push_back(assistant_message);

    session.messages = std::move(completed_messages);
    session.retry_parent_turn_id = session.active_parent_turn_id;
    session.memory_turn_id = session.active_turn_id;
    session.active_turn_id.clear();
    session.active_parent_turn_id.clear();
    session.active_request_messages = json::array();
    session.last_accessed = std::chrono::steady_clock::now();

    session.retry_candidate_hash = session.continuation_hash;
    const std::string next_continuation_hash =
        ChatCompletionUtils::hashConversationPairs(session.messages, false);
    if (!next_continuation_hash.empty()) {
        session.continuation_hash = next_continuation_hash;
    }

    session.has_active_tool_call =
        ChatCompletionUtils::hasToolCalls(assistant_message);
    session.tool_call_hash.clear();
    if (session.has_active_tool_call) {
        session.tool_call_timestamp = std::chrono::steady_clock::now();
        session.tool_call_hash =
            ChatCompletionUtils::hashSpecificMessages(session.messages);
    }

    if (!replay_result.is_null()) {
        session.last_request_signature =
            ChatCompletionUtils::buildRequestSignature(
                request_body, request_messages);
        session.last_replay_result = replay_result;
    } else {
        session.last_request_signature.clear();
        session.last_replay_result = json();
    }

    registerHashMappings(session);
    return true;
}

bool ChatCompletionStore::abortTurn(const std::string& completion_id,
                                    const std::string& turn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = sessions_.find(completion_id);
    if (found == sessions_.end()
        || found->second.active_turn_id != turn_id) {
        return false;
    }

    ChatSession& session = found->second;
    session.active_turn_id.clear();
    session.active_parent_turn_id.clear();
    session.active_request_messages = json::array();
    session.last_accessed = std::chrono::steady_clock::now();
    return true;
}

std::optional<ChatSession> ChatCompletionStore::getSession(
    const std::string& completion_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = sessions_.find(completion_id);
    if (found == sessions_.end()) {
        return std::nullopt;
    }
    return found->second;
}

bool ChatCompletionStore::deleteSession(const std::string& completion_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = sessions_.find(completion_id);
    if (found == sessions_.end()) {
        return false;
    }
    if (!found->second.model.empty()) {
        qai_forge::QaiForge::getInstance().clearSession(
            found->second.model, completion_id);
    }
    unregisterHashMappings(found->second);
    sessions_.erase(found);
    return true;
}

std::vector<std::string> ChatCompletionStore::cleanupExpiredSessions(
    std::chrono::seconds max_age) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::string> expired_ids;

    for (const auto& item : sessions_) {
        const ChatSession& session = item.second;
        const auto age = std::chrono::duration_cast<std::chrono::seconds>(
            now - session.last_accessed);
        if (session.active_turn_id.empty() && age > max_age) {
            expired_ids.push_back(item.first);
        }
    }

    for (const std::string& completion_id : expired_ids) {
        auto found = sessions_.find(completion_id);
        if (found != sessions_.end()) {
            if (!found->second.model.empty()) {
                qai_forge::QaiForge::getInstance().clearSession(
                    found->second.model, completion_id);
            }
            unregisterHashMappings(found->second);
            sessions_.erase(found);
        }
    }
    return expired_ids;
}

size_t ChatCompletionStore::getSessionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

std::vector<std::string> ChatCompletionStore::getAllSessionIds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> ids;
    ids.reserve(sessions_.size());
    for (const auto& item : sessions_) {
        ids.push_back(item.first);
    }
    return ids;
}

void ChatCompletionStore::registerHashMappings(ChatSession& session) {
    if (!session.continuation_hash.empty()) {
        continuation_index_[session.continuation_hash] =
            session.completion_id;
    }
    if (!session.retry_candidate_hash.empty()) {
        retry_index_[session.retry_candidate_hash] = session.completion_id;
    }
    if (!session.tool_call_hash.empty()) {
        tool_call_index_[session.tool_call_hash] = session.completion_id;
    }
}

void ChatCompletionStore::unregisterHashMappings(const ChatSession& session) {
    auto erase_if_owned = [&session](
        std::unordered_map<std::string, std::string>& index,
        const std::string& hash) {
        auto found = index.find(hash);
        if (found != index.end() && found->second == session.completion_id) {
            index.erase(found);
        }
    };

    if (!session.continuation_hash.empty()) {
        erase_if_owned(continuation_index_, session.continuation_hash);
    }
    if (!session.retry_candidate_hash.empty()) {
        erase_if_owned(retry_index_, session.retry_candidate_hash);
    }
    if (!session.tool_call_hash.empty()) {
        erase_if_owned(tool_call_index_, session.tool_call_hash);
    }
}
