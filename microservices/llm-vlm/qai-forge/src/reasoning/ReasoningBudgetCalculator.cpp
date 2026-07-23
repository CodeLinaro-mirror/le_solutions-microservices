// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// ReasoningBudgetCalculator.cpp — Context-aware thinking budget computation
//
// Computes a safe thinking budget from:
//   - The assembled prompt string (for input token estimation)
//   - The model's context window size
//   - The requested reasoning effort level
//   - The client's optional max_output_tokens cap
//
// No metadata.json fields are read here — all policy is in constexpr constants.
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/reasoning/ReasoningBudgetCalculator.h"
#include "qai_forge/utils/Logger.h"
#include <algorithm>
#include <cmath>

// ─────────────────────────────────────────────────────────────────────────────
// effortToFraction — map effort string to fraction of available output tokens
// ─────────────────────────────────────────────────────────────────────────────
float ReasoningBudgetCalculator::effortToFraction(const std::string& effort) {
    if (effort == "none")    return 0.00f;
    if (effort == "minimal") return 0.10f;
    if (effort == "low")     return 0.20f;
    if (effort == "medium")  return 0.35f;
    if (effort == "high")    return 0.50f;
    if (effort == "xhigh")   return 0.60f;
    if (effort == "max")     return 0.70f;
    // Unknown effort → treat as medium (safe default)
    return 0.35f;
}

// ─────────────────────────────────────────────────────────────────────────────
// estimateTokens — character-based token count heuristic
//
// Uses 3.5 chars/token (conservative average for English + code) plus a
// 50-token safety margin. Errs on the side of over-estimating input tokens,
// which safely reduces the thinking budget.
// ─────────────────────────────────────────────────────────────────────────────
int ReasoningBudgetCalculator::estimateTokens(const std::string& text) {
    if (text.empty()) return 50;
    return static_cast<int>(static_cast<float>(text.size()) / 3.5f) + 50;
}

// ─────────────────────────────────────────────────────────────────────────────
// compute — main budget calculation
// ─────────────────────────────────────────────────────────────────────────────
ReasoningBudgetResult ReasoningBudgetCalculator::compute(
    const std::string&  assembled_prompt,
    int                 context_size,
    const std::string&  effort,
    std::optional<int>  max_output_tokens) {

    ReasoningBudgetResult result;
    result.suppress_thinking      = false;
    result.context_too_small      = false;
    result.thinking_budget        = 0;
    result.answer_budget          = 0;
    result.estimated_input_tokens = estimateTokens(assembled_prompt);

    // ── Step 1: effort=none → suppress thinking entirely ─────────────────────
    if (effort == "none") {
        result.suppress_thinking = true;
        result.thinking_budget   = 0;
        // Answer budget = everything available
        int available = std::max(0, context_size - result.estimated_input_tokens);
        result.answer_budget = max_output_tokens.has_value()
            ? std::min(max_output_tokens.value(), available)
            : available;
        LOG_DEBUG("[ReasoningBudgetCalculator] effort=none: thinking suppressed"
                  << " input_est=" << result.estimated_input_tokens
                  << " answer_budget=" << result.answer_budget);
        return result;
    }

    // ── Step 2: Compute available output tokens ───────────────────────────────
    // available_output = context_size - estimated_input - MIN_ANSWER_RESERVE
    int available_output = context_size
                         - result.estimated_input_tokens
                         - MIN_ANSWER_RESERVE;

    if (available_output <= 0) {
        // Context is completely full — can't even fit the minimum answer reserve
        result.context_too_small = true;
        result.suppress_thinking = true;
        result.thinking_budget   = 0;
        result.answer_budget     = 0;
        LOG_WARN("[ReasoningBudgetCalculator] Context too small!"
                 << " context=" << context_size
                 << " estimated_input=" << result.estimated_input_tokens
                 << " min_reserve=" << MIN_ANSWER_RESERVE);
        return result;
    }

    // ── Step 3: Apply max_output_tokens cap ───────────────────────────────────
    // max_output_tokens covers thinking + answer combined (OpenAI spec)
    int effective_output_cap = available_output;
    if (max_output_tokens.has_value() && max_output_tokens.value() > 0) {
        effective_output_cap = std::min(available_output, max_output_tokens.value());
    }

    // If the cap is too small to fit even the minimum answer reserve, suppress
    if (effective_output_cap < MIN_ANSWER_RESERVE) {
        result.suppress_thinking = true;
        result.thinking_budget   = 0;
        result.answer_budget     = effective_output_cap;
        LOG_WARN("[ReasoningBudgetCalculator] max_output_tokens too small for thinking"
                 << " effective_cap=" << effective_output_cap
                 << " min_reserve=" << MIN_ANSWER_RESERVE
                 << " — suppressing thinking");
        return result;
    }

    // ── Step 4: Compute thinking budget ──────────────────────────────────────
    float fraction = effortToFraction(effort);

    // Raw budget = fraction × effective output cap
    int raw_budget = static_cast<int>(fraction * static_cast<float>(effective_output_cap));

    // Apply MAX_THINKING_FRACTION ceiling
    int max_allowed = static_cast<int>(MAX_THINKING_FRACTION
                                       * static_cast<float>(effective_output_cap));

    int thinking_budget = std::min(raw_budget, max_allowed);

    // Ensure answer always gets at least MIN_ANSWER_RESERVE
    thinking_budget = std::min(thinking_budget,
                               effective_output_cap - MIN_ANSWER_RESERVE);

    // Clamp to [0, effective_output_cap]
    thinking_budget = std::max(0, std::min(thinking_budget, effective_output_cap));

    // ── Step 5: Check minimum useful budget ──────────────────────────────────
    if (thinking_budget < MIN_THINKING_TOKENS) {
        // Budget too small to be useful — suppress thinking
        result.suppress_thinking = true;
        result.thinking_budget   = 0;
        result.answer_budget     = effective_output_cap;
        LOG_INFO("[ReasoningBudgetCalculator] Computed thinking budget " << thinking_budget
                 << " < MIN_THINKING_TOKENS " << MIN_THINKING_TOKENS
                 << " for effort='" << effort << "' — suppressing thinking");
        return result;
    }

    // ── Step 6: Final result ──────────────────────────────────────────────────
    result.thinking_budget = thinking_budget;
    result.answer_budget   = effective_output_cap - thinking_budget;
    result.suppress_thinking = false;

    LOG_DEBUG("[ReasoningBudgetCalculator]"
              << " effort=" << effort
              << " context=" << context_size
              << " input_est=" << result.estimated_input_tokens
              << " available=" << available_output
              << " effective_cap=" << effective_output_cap
              << " thinking_budget=" << result.thinking_budget
              << " answer_budget=" << result.answer_budget);

    return result;
}
