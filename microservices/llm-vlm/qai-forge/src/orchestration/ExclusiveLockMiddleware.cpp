// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// ExclusiveLockMiddleware — One inference at a time (DSP/GPU hardware lock)
//
// Wraps ConcurrencyMiddleware::acquire() / release() to enforce the invariant
// that only one session can execute inference at a time.
//
// This replaces the hardcoded ConcurrencyMiddleware::Guard in
// ChatOrchestratorImpl::handleBlocking() and handleStreaming().
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/orchestration/ExclusiveLockMiddleware.h"
#include "qai_forge/orchestration/ConcurrencyMiddleware.h"
#include "qai_forge/utils/Logger.h"

ExclusiveLockMiddleware::ExclusiveLockMiddleware(int timeout_ms)
    : timeout_ms_(timeout_ms), acquired_(false) {}

bool ExclusiveLockMiddleware::before(GenerativeContext& ctx) {
    acquired_ = ConcurrencyMiddleware::getInstance().acquire(
        ctx.session.session_id, timeout_ms_);

    if (!acquired_) {
        LOG_WARN("[ExclusiveLockMiddleware] Timeout acquiring DSP lock for session: "
                 << ctx.session.session_id);
        throw GenAIException(
            GenAIErrorCode::HARDWARE_UNAVAILABLE,
            "The inference hardware is currently busy. "
            "Please retry your request.",
            503);
    }

    LOG_DEBUG("[ExclusiveLockMiddleware] DSP lock acquired for session: "
              << ctx.session.session_id);
    return true;
}

void ExclusiveLockMiddleware::after(GenerativeContext& ctx) {
    if (acquired_) {
        ConcurrencyMiddleware::getInstance().release(ctx.session.session_id);
        acquired_ = false;
        LOG_DEBUG("[ExclusiveLockMiddleware] DSP lock released for session: "
                  << ctx.session.session_id);
    }
}
