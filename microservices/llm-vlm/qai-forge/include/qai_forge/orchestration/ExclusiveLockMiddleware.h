// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/orchestration/IGenerativeMiddleware.h"
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// ExclusiveLockMiddleware — One inference at a time (DSP/GPU hardware lock)
//
// Wraps ConcurrencyMiddleware to enforce the invariant that only one session
// can execute inference at a time. Used by backends with ConcurrencyModel::EXCLUSIVE
// (GenIE, LiteRT LM).
//
// This middleware replaces the hardcoded ConcurrencyMiddleware::Guard that was
// previously embedded directly in ChatOrchestratorImpl::handleBlocking() and
// handleStreaming(). By extracting it into a middleware, the orchestrator can
// build the pipeline dynamically based on BackendCapabilities.
//
// For backends with ConcurrencyModel::BOUNDED or UNLIMITED, this middleware
// is not added to the pipeline — the orchestrator uses BoundedConcurrencyMiddleware
// or no concurrency middleware at all.
//
// See docs/unified-inference-service.md §6 for the full design rationale.
// ─────────────────────────────────────────────────────────────────────────────

class ExclusiveLockMiddleware : public IGenerativeMiddleware {
public:
    /**
     * @param timeout_ms  Max wait time in milliseconds (0 = infinite).
     *                    Throws std::runtime_error if lock not acquired in time.
     *                    Layer 1 maps this to HTTP 503.
     */
    explicit ExclusiveLockMiddleware(int timeout_ms = 300000);
    ~ExclusiveLockMiddleware() override = default;

    std::string name() const override { return "ExclusiveLock"; }

    /**
     * Acquires the DSP/GPU exclusive lock for the session.
     * Blocks until the lock is available or timeout expires.
     * Same-session re-entry (tool continuation) returns immediately.
     * @return true if lock acquired, false if timeout expired.
     */
    bool before(GenerativeContext& ctx) override;

    /**
     * Releases the exclusive lock.
     * Called even if before() returned false (no-op in that case).
     */
    void after(GenerativeContext& ctx) override;

private:
    int  timeout_ms_;
    bool acquired_ = false;
};
