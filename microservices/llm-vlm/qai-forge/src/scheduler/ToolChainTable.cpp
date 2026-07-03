// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/scheduler/ToolChainTable.h"

#include <algorithm>
#include <utility>

namespace scheduler {

namespace {

std::chrono::milliseconds effectiveTtl(std::chrono::milliseconds ttl) {
    return ttl.count() > 0 ? ttl : std::chrono::seconds(30);
}

} // namespace

ToolChainTable::ToolChainTable(
    std::chrono::milliseconds expired_record_retention)
    : expired_record_retention_(
          expired_record_retention.count() > 0
              ? expired_record_retention
              : std::chrono::milliseconds::max()) {}

ToolChainEntry ToolChainTable::open(
    const std::string& response_id,
    const std::string& model_id,
    const std::string& session_id,
    std::chrono::milliseconds ttl,
    std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    cleanupExpiredRecordsLocked(now);

    std::string stored_response_id = response_id;
    if (stored_response_id.empty()) {
        stored_response_id = "generated_response_" +
                             std::to_string(next_generated_id_++);
    }

    auto existing = chain_id_by_response_id_.find(stored_response_id);
    if (existing != chain_id_by_response_id_.end()) {
        auto chain = chains_by_id_.find(existing->second);
        if (chain != chains_by_id_.end()) {
            removeLiveLocked(chain);
        } else {
            chain_id_by_response_id_.erase(existing);
        }
    }
    expired_response_ids_.erase(stored_response_id);

    ToolChainEntry entry;
    entry.chain_id = makeChainIdLocked(stored_response_id);
    entry.response_id = std::move(stored_response_id);
    entry.model_id = model_id;
    entry.session_id = session_id;
    entry.state = ToolChainState::WaitingForToolResult;
    entry.awaiting_tool_result = true;
    entry.created_at = now;
    entry.ttl_deadline = now + effectiveTtl(ttl);

    chain_id_by_response_id_[entry.response_id] = entry.chain_id;
    chains_by_id_[entry.chain_id] = entry;
    return entry;
}

ToolChainResolveResult ToolChainTable::resolveByPreviousResponseId(
    const std::string& previous_response_id,
    std::chrono::steady_clock::time_point now) {
    if (previous_response_id.empty()) {
        return ToolChainResolveResult{
            ToolChainResolveStatus::NotFound,
            std::nullopt,
            "previous_response_id is empty",
        };
    }

    std::lock_guard<std::mutex> lock(mutex_);
    cleanupExpiredRecordsLocked(now);

    if (expired_response_ids_.count(previous_response_id) > 0) {
        return ToolChainResolveResult{
            ToolChainResolveStatus::Expired,
            std::nullopt,
            "Tool response window expired for previous_response_id '" +
                previous_response_id + "'",
        };
    }

    auto mapped_chain = chain_id_by_response_id_.find(previous_response_id);
    if (mapped_chain == chain_id_by_response_id_.end()) {
        return ToolChainResolveResult{
            ToolChainResolveStatus::NotFound,
            std::nullopt,
            "No pending tool chain found for previous_response_id '" +
                previous_response_id + "'",
        };
    }

    auto chain = chains_by_id_.find(mapped_chain->second);
    if (chain == chains_by_id_.end()) {
        chain_id_by_response_id_.erase(mapped_chain);
        return ToolChainResolveResult{
            ToolChainResolveStatus::NotFound,
            std::nullopt,
            "No pending tool chain found for previous_response_id '" +
                previous_response_id + "'",
        };
    }

    if (now >= chain->second.ttl_deadline) {
        ToolChainEntry expired = moveToExpiredLocked(chain, now);
        return ToolChainResolveResult{
            ToolChainResolveStatus::Expired,
            expired,
            "Tool response window expired for previous_response_id '" +
                previous_response_id + "'",
        };
    }

    return ToolChainResolveResult{
        ToolChainResolveStatus::Found,
        chain->second,
        "found",
    };
}

bool ToolChainTable::markContinuationQueued(
    const std::string& chain_id,
    const std::string& job_id,
    std::chrono::milliseconds ttl,
    std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    cleanupExpiredRecordsLocked(now);

    auto chain = chains_by_id_.find(chain_id);
    if (chain == chains_by_id_.end()) {
        return false;
    }

    if (now >= chain->second.ttl_deadline) {
        moveToExpiredLocked(chain, now);
        return false;
    }

    chain->second.state = ToolChainState::ContinuationQueued;
    chain->second.awaiting_tool_result = false;
    chain->second.pending_job_id = job_id;
    chain->second.ttl_deadline = now + effectiveTtl(ttl);
    return true;
}

bool ToolChainTable::close(const std::string& chain_id) {
    if (chain_id.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto chain = chains_by_id_.find(chain_id);
    if (chain == chains_by_id_.end()) {
        return false;
    }

    removeLiveLocked(chain);
    return true;
}

std::vector<ToolChainEntry> ToolChainTable::expireStale(
    std::chrono::steady_clock::time_point now) {
    std::vector<ToolChainEntry> expired;

    std::lock_guard<std::mutex> lock(mutex_);
    cleanupExpiredRecordsLocked(now);

    for (auto it = chains_by_id_.begin(); it != chains_by_id_.end();) {
        if (now < it->second.ttl_deadline) {
            ++it;
            continue;
        }

        expired.push_back(moveToExpiredLocked(it++, now));
    }

    return expired;
}

std::vector<ToolChainEntry> ToolChainTable::snapshot() const {
    std::vector<ToolChainEntry> entries;

    std::lock_guard<std::mutex> lock(mutex_);
    entries.reserve(chains_by_id_.size());
    for (const auto& entry : chains_by_id_) {
        entries.push_back(entry.second);
    }

    std::sort(
        entries.begin(),
        entries.end(),
        [](const ToolChainEntry& lhs, const ToolChainEntry& rhs) {
            if (lhs.ttl_deadline != rhs.ttl_deadline) {
                return lhs.ttl_deadline < rhs.ttl_deadline;
            }
            return lhs.chain_id < rhs.chain_id;
        });
    return entries;
}

size_t ToolChainTable::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return chains_by_id_.size();
}

std::string ToolChainTable::makeChainIdLocked(
    const std::string& response_id) {
    std::string base = "chain_" + response_id;
    if (base == "chain_") {
        base = "chain_generated";
    }

    std::string candidate = base;
    while (chains_by_id_.count(candidate) > 0) {
        candidate = base + "_" + std::to_string(next_generated_id_++);
    }
    return candidate;
}

void ToolChainTable::removeLiveLocked(ChainMap::iterator it) {
    chain_id_by_response_id_.erase(it->second.response_id);
    chains_by_id_.erase(it);
}

ToolChainEntry ToolChainTable::moveToExpiredLocked(
    ChainMap::iterator it,
    std::chrono::steady_clock::time_point now) {
    ToolChainEntry expired = it->second;
    chain_id_by_response_id_.erase(expired.response_id);
    chains_by_id_.erase(it);
    auto forget_at = std::chrono::steady_clock::time_point::max();
    if (expired_record_retention_ != std::chrono::milliseconds::max() &&
        std::chrono::steady_clock::time_point::max() - now >
            expired_record_retention_) {
        forget_at = now + expired_record_retention_;
    }
    expired_response_ids_[expired.response_id] = ExpiredRecord{
        now,
        forget_at,
    };
    return expired;
}

void ToolChainTable::cleanupExpiredRecordsLocked(
    std::chrono::steady_clock::time_point now) {
    for (auto it = expired_response_ids_.begin();
         it != expired_response_ids_.end();) {
        if (now >= it->second.forget_at) {
            it = expired_response_ids_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace scheduler
