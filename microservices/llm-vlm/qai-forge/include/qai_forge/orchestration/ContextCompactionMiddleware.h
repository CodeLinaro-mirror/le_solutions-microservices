// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/orchestration/IGenerativeMiddleware.h"
#include "qai_forge/backend/BackendCapabilities.h"
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// ContextCompactionMiddleware — Summarize when context window is full
//
// Checks if the projected token count exceeds the compaction threshold.
// If so, generates a summary and calls backend.onContextCompacted() to
// reset the KV cache (for RESET_KV backends) or do nothing (FULL_RECOMPUTE).
//
// This replaces the hardcoded SummarizationMiddleware::checkAndSummarize()
// call in ChatOrchestratorImpl::handleBlocking() and handleStreaming().
// By extracting it into a middleware, the orchestrator can build the pipeline
// dynamically based on BackendCapabilities.context_strategy.
//
// Context strategy determines post-summarization behavior:
//   RESET_KV:       calls backend.onContextCompacted() → sendReset()
//   FULL_RECOMPUTE: calls backend.onContextCompacted() → no-op
//   SLIDING_WINDOW: future — not yet implemented
//
// See docs/unified-inference-service.md §6 for the full design rationale.
// ─────────────────────────────────────────────────────────────────────────────

class ContextCompactionMiddleware : public IGenerativeMiddleware {
public:
    /**
     * @param context_window       Max tokens this backend supports
     * @param compaction_threshold Fraction of context_window that triggers
     *                             summarization (e.g. 0.70 = 70%)
     * @param strategy             How the backend manages context after compaction
     */
    ContextCompactionMiddleware(int             context_window,
                                float           compaction_threshold,
                                ContextStrategy strategy);
    ~ContextCompactionMiddleware() override = default;

    std::string name() const override { return "ContextCompaction"; }

    /**
     * Checks if summarization is needed and performs it if so.
     * Calls backend.onContextCompacted() after summarization.
     * Always returns true (compaction is non-fatal — inference continues
     * with the compacted context even if summarization fails).
     */
    bool before(GenerativeContext& ctx) override;

    /**
     * No-op — context compaction has no post-inference cleanup.
     */
    void after(GenerativeContext& ctx) override {}

private:
    int             context_window_;
    float           compaction_threshold_;
    ContextStrategy strategy_;
};
