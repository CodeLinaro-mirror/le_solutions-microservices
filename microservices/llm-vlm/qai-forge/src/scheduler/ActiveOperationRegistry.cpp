// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/scheduler/ActiveOperationRegistry.h"

#include "qai_forge/InternalDTOs.h"

namespace scheduler {

void ActiveOperationRegistry::registerOperation(const GenerativeJob& job) {
    if (job.response_id.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = operations_.find(job.response_id);
    if (found != operations_.end()) {
        if (found->second.cancelled) {
            throw GenAIException(
                GenAIErrorCode::INVALID_REQUEST,
                "Request was cancelled and cannot be resumed",
                409);
        }
        if (!found->second.job_id.empty() &&
            found->second.job_id != job.job_id) {
            throw GenAIException(
                GenAIErrorCode::INVALID_REQUEST,
                "Another scheduler job is already active for this request",
                409);
        }
    }

    const bool is_mcp_round = job.kind == JobKind::MCP_ROUND;
    Operation& operation = operations_[job.response_id];
    operation.job_id = job.job_id;
    operation.lease_model_id = is_mcp_round ? job.model_id : std::string();
    operation.lease_id =
        is_mcp_round ? ("internal_mcp_" + job.response_id) : std::string();
    operation.session_id = job.session_id;
    operation.memory_turn = job.memory_turn;
    operation.cancelled = false;
    operation.last_activity = std::chrono::steady_clock::now();
}

bool ActiveOperationRegistry::finishOperation(const std::string& response_id,
                                              const std::string& job_id,
                                              bool keep_for_resume) {
    if (response_id.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = operations_.find(response_id);
    if (found == operations_.end() || found->second.job_id != job_id) {
        return false;
    }
    if (keep_for_resume && !found->second.cancelled) {
        found->second.job_id.clear();
        found->second.last_activity = std::chrono::steady_clock::now();
        return true;
    }
    operations_.erase(found);
    return false;
}

ActiveOperationRegistry::CancelInfo ActiveOperationRegistry::markCancelled(
    const std::string& response_id) {
    CancelInfo info;
    if (response_id.empty()) {
        return info;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = operations_.find(response_id);
    if (found == operations_.end()) {
        return info;
    }
    found->second.cancelled = true;
    found->second.last_activity = std::chrono::steady_clock::now();
    info.found = true;
    info.job_id = found->second.job_id;
    info.lease_model_id = found->second.lease_model_id;
    info.lease_id = found->second.lease_id;
    info.session_id = found->second.session_id;
    info.memory_turn = found->second.memory_turn;
    return info;
}

std::vector<std::pair<std::string, std::string>>
ActiveOperationRegistry::expireIdle(std::chrono::milliseconds ttl) {
    std::vector<std::pair<std::string, std::string>> expired_leases;
    const auto cutoff = std::chrono::steady_clock::now() - ttl;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = operations_.begin(); it != operations_.end();) {
        if (it->second.job_id.empty() && it->second.last_activity <= cutoff) {
            expired_leases.emplace_back(
                it->second.lease_model_id, it->second.lease_id);
            it = operations_.erase(it);
        } else {
            ++it;
        }
    }
    return expired_leases;
}

} // namespace scheduler
