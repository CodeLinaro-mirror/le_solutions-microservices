// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// ContextCompactionMiddleware — Summarize when context window is full
//
// Delegates to SummarizationMiddleware::checkAndSummarize() which calls
// backend.generate() for the summary and backend.onContextCompacted() after.
//
// This replaces the hardcoded SummarizationMiddleware::checkAndSummarize()
// call in ChatOrchestratorImpl::handleBlocking() and handleStreaming().
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/orchestration/ContextCompactionMiddleware.h"
#include "qai_forge/orchestration/SummarizationMiddleware.h"
#include "qai_forge/utils/Logger.h"

ContextCompactionMiddleware::ContextCompactionMiddleware(
    int             context_window,
    float           compaction_threshold,
    ContextStrategy strategy)
    : context_window_(context_window)
    , compaction_threshold_(compaction_threshold)
    , strategy_(strategy)
{}

bool ContextCompactionMiddleware::before(GenerativeContext& ctx) {
    // Delegate to SummarizationMiddleware which handles:
    //   1. Token count estimation
    //   2. Summary generation via backend.generate()
    //   3. Session update (session.summary_content)
    //   4. backend.onContextCompacted() — GenIE: sendReset(); OnnxRT: no-op
    bool compacted = SummarizationMiddleware::checkAndSummarize(
        ctx.session,
        ctx.request,
        ctx.backend,
        context_window_,
        compaction_threshold_);

    if (compacted) {
        LOG_INFO("[ContextCompactionMiddleware] Context compacted for session: "
                 << ctx.session.session_id
                 << " (strategy=" << (strategy_ == ContextStrategy::RESET_KV
                                      ? "RESET_KV" : "FULL_RECOMPUTE") << ")");
    }

    // Always continue the pipeline — compaction is non-fatal
    return true;
}
