// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ResponseStore.h"

#include <algorithm>
#include <chrono>

ResponseStore& ResponseStore::getInstance() {
    static ResponseStore store;
    return store;
}

BeginResponseResult ResponseStore::beginResponse(
    const std::string& response_id,
    const std::string& model,
    const std::string& previous_response_id,
    const ResponseStoreJson& input_items,
    const ResponseStoreJson& request_messages,
    const ResponseStoreJson& metadata,
    int ttl_seconds) {
    std::lock_guard<std::mutex> lock(mu_);

    if (responses_by_id_.find(response_id) != responses_by_id_.end()) {
        return makeBeginError(
            409,
            "Response " + response_id + " already exists");
    }

    std::string session_id;
    if (!previous_response_id.empty()) {
        auto parent_it = responses_by_id_.find(previous_response_id);
        if (parent_it == responses_by_id_.end()) {
            return makeBeginError(
                404,
                "Previous response " + previous_response_id + " not found");
        }

        const StoredResponse& parent = parent_it->second;
        if (parent.status == StoredResponseStatus::InProgress) {
            return makeBeginError(
                409,
                "Previous response " + previous_response_id
                    + " is still in progress");
        }
        if (parent.status != StoredResponseStatus::Completed) {
            return makeBeginError(
                404,
                "Previous response " + previous_response_id
                    + " is not continuable");
        }
        session_id = parent.session_id;
    } else {
        session_id = "rsess_" + response_id;
    }

    int now = currentUnixTime();

    StoredResponse response;
    response.response_id = response_id;
    response.session_id = session_id;
    response.previous_response_id = previous_response_id;
    response.model = model;
    response.status = StoredResponseStatus::InProgress;
    response.input_items = input_items;
    response.request_messages = request_messages;
    response.metadata = metadata.is_null() ? ResponseStoreJson::object() : metadata;
    response.active_job_id = response_id;
    response.created_at = now;
    response.updated_at = now;
    response.expires_at = now + ttl_seconds;

    auto& session = sessions_by_id_[session_id];
    if (session.session_id.empty()) {
        session.session_id = session_id;
        session.root_response_id = response_id;
        session.created_at = now;
    }
    session.response_ids.insert(response_id);
    session.last_activity_at = now;

    responses_by_id_[response_id] = response;
    if (!previous_response_id.empty()) {
        children_by_response_id_[previous_response_id].push_back(response_id);
    }

    BuildCandidateResult walk =
        buildCandidateMessagesLocked(previous_response_id, request_messages);
    if (!walk.ok) {
        responses_by_id_.erase(response_id);
        session.response_ids.erase(response_id);
        if (session.response_ids.empty()) {
            sessions_by_id_.erase(session_id);
        }
        if (!previous_response_id.empty()) {
            auto children_it = children_by_response_id_.find(previous_response_id);
            if (children_it != children_by_response_id_.end()) {
                auto& children = children_it->second;
                children.erase(
                    std::remove(children.begin(), children.end(), response_id),
                    children.end());
                if (children.empty()) {
                    children_by_response_id_.erase(children_it);
                }
            }
        }
        return makeBeginError(walk.http_status, walk.error_message);
    }

    BeginResponseResult result;
    result.ok = true;
    result.response_id = response_id;
    result.session_id = session_id;
    result.created_at = now;
    result.ancestor_messages = walk.ancestor_messages;
    result.current_request_messages = walk.current_request_messages;
    return result;
}

bool ResponseStore::completeResponse(
    const std::string& response_id,
    const ResponseStoreJson& assistant_messages,
    const ResponseStoreJson& output_items,
    const ResponseStoreJson& response_object,
    const ResponseStoreJson& usage) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = responses_by_id_.find(response_id);
    if (it == responses_by_id_.end()
        || it->second.status != StoredResponseStatus::InProgress) {
        return false;
    }

    int now = currentUnixTime();
    StoredResponse& response = it->second;
    response.assistant_messages = assistant_messages;
    response.output_items = output_items;
    response.response_object = response_object;
    response.usage = usage;
    response.error = nullptr;
    response.incomplete_details = nullptr;
    response.status = StoredResponseStatus::Completed;
    response.active_job_id.clear();
    response.updated_at = now;
    response.completed_at = now;

    auto session_it = sessions_by_id_.find(response.session_id);
    if (session_it != sessions_by_id_.end()) {
        session_it->second.last_activity_at = now;
    }
    return true;
}

bool ResponseStore::failResponse(const std::string& response_id,
                                 const ResponseStoreJson& error_object) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = responses_by_id_.find(response_id);
    if (it == responses_by_id_.end()
        || it->second.status != StoredResponseStatus::InProgress) {
        return false;
    }

    int now = currentUnixTime();
    StoredResponse& response = it->second;
    response.status = StoredResponseStatus::Failed;
    response.error = error_object.is_null()
        ? ResponseStoreJson({{"code", "server_error"}, {"message", "Response failed"}})
        : error_object;
    response.incomplete_details = nullptr;
    response.output_items = ResponseStoreJson::array();
    response.usage = nullptr;
    response.active_job_id.clear();
    response.updated_at = now;
    response.completed_at = now;
    response.response_object = makeResponseObject(
        response,
        "failed",
        response.output_items,
        response.usage,
        response.error,
        ResponseStoreJson(nullptr));
    return true;
}

CancelOutcome ResponseStore::cancelResponse(const std::string& response_id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = responses_by_id_.find(response_id);
    if (it == responses_by_id_.end()) {
        return CancelOutcome::NotFound;
    }
    if (it->second.status != StoredResponseStatus::InProgress) {
        return CancelOutcome::InvalidState;
    }

    int now = currentUnixTime();
    StoredResponse& response = it->second;
    response.status = StoredResponseStatus::Cancelled;
    response.error = nullptr;
    response.incomplete_details = nullptr;
    response.output_items = ResponseStoreJson::array();
    response.usage = nullptr;
    response.active_job_id.clear();
    response.updated_at = now;
    response.completed_at = now;
    response.response_object = makeResponseObject(
        response,
        "cancelled",
        response.output_items,
        response.usage,
        ResponseStoreJson(nullptr),
        ResponseStoreJson(nullptr));
    return CancelOutcome::Cancelled;
}

DeleteCascadeResult ResponseStore::deleteCascade(
    const std::string& response_id) {
    std::lock_guard<std::mutex> lock(mu_);
    if (responses_by_id_.find(response_id) == responses_by_id_.end()) {
        DeleteCascadeResult result;
        result.ok = false;
        result.http_status = 404;
        result.error_message = "Response " + response_id + " not found";
        return result;
    }

    std::vector<std::string> subtree;
    std::vector<std::string> stack = {response_id};
    while (!stack.empty()) {
        std::string current = stack.back();
        stack.pop_back();
        subtree.push_back(current);

        auto children_it = children_by_response_id_.find(current);
        if (children_it != children_by_response_id_.end()) {
            for (const auto& child : children_it->second) {
                stack.push_back(child);
            }
        }
    }

    for (const auto& id : subtree) {
        auto it = responses_by_id_.find(id);
        if (it != responses_by_id_.end()
            && it->second.status == StoredResponseStatus::InProgress) {
            DeleteCascadeResult result;
            result.ok = false;
            result.http_status = 409;
            result.error_message =
                "Cancel response " + id + " before deleting this subtree";
            return result;
        }
    }

    for (const auto& id : subtree) {
        auto it = responses_by_id_.find(id);
        if (it == responses_by_id_.end()) {
            continue;
        }

        std::string session_id = it->second.session_id;
        std::string parent_id = it->second.previous_response_id;
        if (!parent_id.empty()) {
            auto parent_children_it = children_by_response_id_.find(parent_id);
            if (parent_children_it != children_by_response_id_.end()) {
                auto& children = parent_children_it->second;
                children.erase(
                    std::remove(children.begin(), children.end(), id),
                    children.end());
                if (children.empty()) {
                    children_by_response_id_.erase(parent_children_it);
                }
            }
        }

        children_by_response_id_.erase(id);

        auto session_it = sessions_by_id_.find(session_id);
        if (session_it != sessions_by_id_.end()) {
            session_it->second.response_ids.erase(id);
            if (session_it->second.response_ids.empty()) {
                sessions_by_id_.erase(session_it);
            }
        }

        responses_by_id_.erase(it);
    }

    DeleteCascadeResult result;
    result.ok = true;
    result.http_status = 200;
    return result;
}

std::vector<ExpiredResponse> ResponseStore::expireStaleInProgress(int now_unix) {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<ExpiredResponse> expired;

    for (auto& entry : responses_by_id_) {
        StoredResponse& response = entry.second;
        if (response.status != StoredResponseStatus::InProgress
            || response.expires_at <= 0
            || response.expires_at > now_unix) {
            continue;
        }

        ExpiredResponse expired_response;
        expired_response.response_id = response.response_id;
        expired_response.active_job_id = response.active_job_id;
        expired.push_back(expired_response);

        response.status = StoredResponseStatus::Expired;
        response.error = {
            {"code", "server_error"},
            {"message", "Response expired before completion"}
        };
        response.incomplete_details = nullptr;
        response.output_items = ResponseStoreJson::array();
        response.usage = nullptr;
        response.active_job_id.clear();
        response.updated_at = now_unix;
        response.completed_at = now_unix;
        response.response_object = makeResponseObject(
            response,
            "failed",
            response.output_items,
            response.usage,
            response.error,
            ResponseStoreJson(nullptr));
    }

    return expired;
}

std::optional<StoredResponse> ResponseStore::getResponse(
    const std::string& response_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = responses_by_id_.find(response_id);
    if (it == responses_by_id_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<std::string> ResponseStore::getActiveJob(
    const std::string& response_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = responses_by_id_.find(response_id);
    if (it == responses_by_id_.end()) {
        return std::nullopt;
    }

    const StoredResponse& response = it->second;
    if (response.status != StoredResponseStatus::InProgress
        || response.active_job_id.empty()) {
        return std::nullopt;
    }
    return response.active_job_id;
}

std::optional<ResponseStoreJson> ResponseStore::getInputItems(
    const std::string& response_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = responses_by_id_.find(response_id);
    if (it == responses_by_id_.end()) {
        return std::nullopt;
    }
    return it->second.input_items;
}

BuildCandidateResult ResponseStore::buildCandidateMessages(
    const std::string& previous_response_id,
    const ResponseStoreJson& request_messages) const {
    std::lock_guard<std::mutex> lock(mu_);
    return buildCandidateMessagesLocked(previous_response_id, request_messages);
}

BuildCandidateResult ResponseStore::buildCandidateMessagesLocked(
    const std::string& previous_response_id,
    const ResponseStoreJson& request_messages) const {
    BuildCandidateResult result;
    result.ok = true;
    result.current_request_messages = request_messages;

    if (previous_response_id.empty()) {
        return result;
    }

    std::vector<const StoredResponse*> path;
    std::string cursor = previous_response_id;
    while (!cursor.empty()) {
        auto it = responses_by_id_.find(cursor);
        if (it == responses_by_id_.end()) {
            return makeCandidateError(
                404,
                "Previous response " + cursor + " not found",
                request_messages);
        }

        const StoredResponse& response = it->second;
        if (response.status == StoredResponseStatus::InProgress) {
            return makeCandidateError(
                409,
                "Previous response " + cursor + " is still in progress",
                request_messages);
        }
        if (response.status != StoredResponseStatus::Completed) {
            return makeCandidateError(
                404,
                "Previous response " + cursor + " is not continuable",
                request_messages);
        }

        if (result.session_id.empty()) {
            result.session_id = response.session_id;
        }
        path.push_back(&response);
        cursor = response.previous_response_id;
    }

    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        appendMessages(result.ancestor_messages, (*it)->request_messages);
        appendMessages(result.ancestor_messages, (*it)->assistant_messages);
    }

    return result;
}

void ResponseStore::clearForTest() {
    std::lock_guard<std::mutex> lock(mu_);
    responses_by_id_.clear();
    sessions_by_id_.clear();
    children_by_response_id_.clear();
}

int ResponseStore::currentUnixTime() {
    return static_cast<int>(
        std::chrono::system_clock::now().time_since_epoch().count()
        / 1000000000LL);
}

void ResponseStore::appendMessages(ResponseStoreJson& destination,
                                   const ResponseStoreJson& messages) {
    if (messages.is_array()) {
        for (const auto& message : messages) {
            destination.push_back(message);
        }
        return;
    }

    if (!messages.is_null()) {
        destination.push_back(messages);
    }
}

ResponseStoreJson ResponseStore::makeResponseObject(
    const StoredResponse& response,
    const std::string& public_status,
    const ResponseStoreJson& output_items,
    const ResponseStoreJson& usage,
    const ResponseStoreJson& error,
    const ResponseStoreJson& incomplete_details) {
    return {
        {"id", response.response_id},
        {"object", "response"},
        {"created_at", response.created_at},
        {"model", response.model},
        {"status", public_status},
        {"output", output_items},
        {"usage", usage},
        {"error", error.is_null() ? ResponseStoreJson(nullptr) : error},
        {"incomplete_details", incomplete_details.is_null()
            ? ResponseStoreJson(nullptr)
            : incomplete_details},
        {"previous_response_id", response.previous_response_id.empty()
            ? ResponseStoreJson(nullptr)
            : ResponseStoreJson(response.previous_response_id)},
        {"metadata", response.metadata.is_null()
            ? ResponseStoreJson::object()
            : response.metadata}
    };
}

BuildCandidateResult ResponseStore::makeCandidateError(
    int http_status,
    const std::string& error_message,
    const ResponseStoreJson& request_messages) {
    BuildCandidateResult result;
    result.ok = false;
    result.http_status = http_status;
    result.error_message = error_message;
    result.current_request_messages = request_messages;
    return result;
}

BeginResponseResult ResponseStore::makeBeginError(
    int http_status,
    const std::string& error_message) {
    BeginResponseResult result;
    result.ok = false;
    result.http_status = http_status;
    result.error_message = error_message;
    return result;
}
