// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// ReasoningBudgetCalculator — Context-aware thinking budget computation
//
// On-device models have small context windows (4096–8192 tokens). This class
// computes a safe thinking budget that:
//   1. Fits within the available context after accounting for the assembled prompt
//   2. Scales with the requested reasoning effort level
//   3. Always reserves a minimum number of tokens for the answer
//   4. Respects the client's max_output_tokens cap
//
// All policy constants are application-level (not in metadata.json):
//   MIN_ANSWER_RESERVE    = 256   tokens always reserved for the answer
//   MAX_THINKING_FRACTION = 0.60  thinking never > 60% of available output
//   MIN_THINKING_TOKENS   = 64    minimum useful thinking budget
//
// Effort → fraction of available output tokens:
//   none    = 0.00  (bypass_think_filter=false, thinking suppressed)
//   minimal = 0.10
//   low     = 0.20
//   medium  = 0.35  (default when no reasoning.effort specified)
//   high    = 0.50
//   xhigh   = 0.60  (equals MAX_THINKING_FRACTION)
//
// Token estimation uses a character-based heuristic (3.5 chars/token + 50
// safety margin). This is fast, requires no tokenizer, and errs on the side
// of over-estimating input tokens (which safely reduces the thinking budget).
//
// Usage:
//   std::string prompt = buildContextPrompt(*session, request);
//   auto budget = ReasoningBudgetCalculator::compute(
//       prompt,
//       ModelConfigManager::getInstance().getContextSize(model_id),
//       request.reasoning_effort.value_or("medium"),
//       request.max_completion_tokens
//   );
//   if (budget.context_too_small) throw ...;
//   if (budget.suppress_thinking) bypass_think_filter = false;
//   // Use budget.thinking_budget for ReasoningRouter
//   // Use budget.answer_budget for max_completion_tokens in executeRequest()
// ─────────────────────────────────────────────────────────────────────────────

#include <string>
#include <optional>

// ─────────────────────────────────────────────────────────────────────────────
// ReasoningBudgetResult — output of ReasoningBudgetCalculator::compute()
// ─────────────────────────────────────────────────────────────────────────────
struct ReasoningBudgetResult {
    int  thinking_budget;        // tokens for ReasoningBudgetTracker (-1 = unlimited)
    int  answer_budget;          // tokens for max_completion_tokens in executeRequest()
    bool suppress_thinking;      // true when effort=none or context too small for thinking
    int  estimated_input_tokens; // estimated prompt token count (for logging)
    bool context_too_small;      // true if even MIN_ANSWER_RESERVE cannot fit
};

// ─────────────────────────────────────────────────────────────────────────────
// ReasoningBudgetCalculator
// ─────────────────────────────────────────────────────────────────────────────
class ReasoningBudgetCalculator {
public:
    // ── Application-level policy constants ───────────────────────────────────
    // These are NOT in metadata.json — they are server-side policy.

    /// Minimum tokens always reserved for the answer (never given to thinking).
    static constexpr int   MIN_ANSWER_RESERVE    = 256;

    /// Thinking tokens never exceed this fraction of available output tokens.
    static constexpr float MAX_THINKING_FRACTION = 0.60f;

    /// Minimum useful thinking budget. If the computed budget is below this
    /// and effort != none, thinking is suppressed (not worth the overhead).
    static constexpr int   MIN_THINKING_TOKENS   = 64;

    // ── Main computation ──────────────────────────────────────────────────────

    /**
     * Compute the thinking budget for a reasoning model request.
     *
     * @param assembled_prompt    The full prompt string from buildContextPrompt()
     *                            (includes system prompt, history, tool defs, user msg)
     * @param context_size        Model's context window size in tokens
     *                            (from ModelConfigManager::getContextSize())
     * @param effort              Reasoning effort level: "none"/"minimal"/"low"/
     *                            "medium"/"high"/"xhigh"
     *                            Unknown values are treated as "medium".
     * @param max_output_tokens   Optional client-requested output token cap.
     *                            Covers both thinking + answer tokens combined.
     * @param max_reasoning_tokens Optional explicit cap on thinking tokens only.
     *                            Takes precedence over effort-based calculation.
     * @return                    ReasoningBudgetResult with all computed values
     */
    static ReasoningBudgetResult compute(
        const std::string&   assembled_prompt,
        int                  context_size,
        const std::string&   effort,
        std::optional<int>   max_output_tokens = std::nullopt,
        std::optional<int>   max_reasoning_tokens = std::nullopt
    );

    // ── Helpers (exposed for testing) ─────────────────────────────────────────

    /**
     * Map an effort string to a fraction of available output tokens.
     * Returns 0.0 for "none", 0.35 for unknown values (medium default).
     */
    static float effortToFraction(const std::string& effort);

    /**
     * Estimate the number of tokens in a text string.
     * Uses: floor(text.size() / 3.5) + 50 (safety margin).
     * Errs on the side of over-estimating (safer for budget computation).
     */
    static int estimateTokens(const std::string& text);
};
