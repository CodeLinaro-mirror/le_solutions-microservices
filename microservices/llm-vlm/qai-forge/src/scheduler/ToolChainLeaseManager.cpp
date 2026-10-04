// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/scheduler/ToolChainLeaseManager.h"

#include "qai_forge/scheduler/InferenceScheduler.h"

#include <utility>

namespace scheduler {

ToolChainLeaseManager::ToolChainLeaseManager(InferenceScheduler& scheduler)
    : scheduler_(scheduler) {}

ToolChainResolveResult ToolChainLeaseManager::resolveByPreviousResponseId(
    const std::string& previous_response_id) {
    return tool_chains_.resolveByPreviousResponseId(previous_response_id);
}

bool ToolChainLeaseManager::markContinuationQueuedAndRenew(
    const std::string& chain_id,
    const std::string& job_id,
    const std::string& model_id,
    std::chrono::milliseconds ttl,
    const std::string& session_id) {
    if (!tool_chains_.markContinuationQueued(chain_id, job_id, ttl)) {
        return false;
    }
    scheduler_.renewToolLease(model_id, chain_id, ttl, session_id);
    return true;
}

void ToolChainLeaseManager::renewLease(const std::string& model_id,
                                       const std::string& chain_id,
                                       std::chrono::milliseconds ttl,
                                       const std::string& session_id) {
    scheduler_.renewToolLease(model_id, chain_id, ttl, session_id);
}

ToolChainEntry ToolChainLeaseManager::openAndLease(
    const std::string& response_id,
    const std::string& model_id,
    const std::string& session_id,
    std::chrono::milliseconds ttl) {
    ToolChainEntry chain =
        tool_chains_.open(response_id, model_id, session_id, ttl);
    // Open the new lease before the caller closes the previous chain, so
    // the lease is never momentarily absent (which would make the
    // runtime eviction-eligible).
    scheduler_.openToolLease(chain.model_id, chain.chain_id, ttl, session_id);
    return chain;
}

void ToolChainLeaseManager::openLease(const std::string& model_id,
                                      const std::string& chain_id,
                                      std::chrono::milliseconds ttl,
                                      const std::string& session_id) {
    scheduler_.openToolLease(model_id, chain_id, ttl, session_id);
}

void ToolChainLeaseManager::closeChain(const std::string& model_id,
                                       const std::string& chain_id,
                                       const std::string& session_id) {
    if (chain_id.empty()) {
        return;
    }
    tool_chains_.close(chain_id);
    scheduler_.closeToolLease(model_id, chain_id, session_id);
}

void ToolChainLeaseManager::expireStale() {
    const std::vector<ToolChainEntry> expired = tool_chains_.expireStale();
    for (const ToolChainEntry& chain : expired) {
        scheduler_.closeToolLease(chain.model_id, chain.chain_id, chain.session_id);
    }
}

} // namespace scheduler
