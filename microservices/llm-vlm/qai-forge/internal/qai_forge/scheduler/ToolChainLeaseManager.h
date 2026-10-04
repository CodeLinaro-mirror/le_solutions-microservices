// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/scheduler/ToolChainTable.h"

#include <chrono>
#include <string>

namespace scheduler {

class InferenceScheduler;

// Single owner of tool-chain identity and its paired scheduler-level lease:
// a tool-chain's identity (ToolChainTable) and its WarmModelPool-visible
// lease must always be opened/closed together, and this class enforces
// that invariant.
//
// Internal-MCP-round leases (see QaiForge::Impl::internalMcpLeaseId()) use a
// synthetic chain id that is never stored in the underlying ToolChainTable
// (there is no externally-visible previous_response_id to resolve later) —
// renewLease()/openLease() exist for that lease-only path, where the caller
// manages the chain_id string itself.
class ToolChainLeaseManager {
public:
    explicit ToolChainLeaseManager(InferenceScheduler& scheduler);

    // Resolve a previous_response_id to its tool-chain entry. Pure lookup —
    // does not touch any scheduler lease.
    ToolChainResolveResult resolveByPreviousResponseId(
        const std::string& previous_response_id);

    // Marks the chain's continuation as queued in the ToolChainTable and, iff
    // that succeeds, renews its scheduler lease. Returns false (with no
    // lease renewal attempted) if the continuation could not be marked
    // (e.g. the chain already expired).
    bool markContinuationQueuedAndRenew(const std::string& chain_id,
                                        const std::string& job_id,
                                        const std::string& model_id,
                                        std::chrono::milliseconds ttl);

    // Renews an existing scheduler lease without touching the
    // ToolChainTable. Used for internal-MCP-round leases.
    void renewLease(const std::string& model_id,
                    const std::string& chain_id,
                    std::chrono::milliseconds ttl);

    // Opens a brand-new tool chain for a response that just produced
    // tool_calls, and opens its matching scheduler lease.
    ToolChainEntry openAndLease(const std::string& response_id,
                                const std::string& model_id,
                                const std::string& session_id,
                                std::chrono::milliseconds ttl);

    // Opens just the scheduler lease, with no ToolChainTable entry. Used for
    // internal-MCP-round leases.
    void openLease(const std::string& model_id,
                   const std::string& chain_id,
                   std::chrono::milliseconds ttl);

    // Closes a chain: closes both the ToolChainTable entry (if any) and the
    // scheduler lease. No-op if chain_id is empty.
    void closeChain(const std::string& model_id,
                    const std::string& chain_id);

    // Sweeps expired ToolChainTable entries and closes their scheduler
    // leases.
    void expireStale();

private:
    InferenceScheduler& scheduler_;
    ToolChainTable tool_chains_;
};

} // namespace scheduler
