// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// TokenEstimation — shared "chars/4" token heuristic + generic front-eviction
//
// Every generative orchestrator (GenieOrchestrator, LiteRTLMOrchestrator,
// LlamaCppOrchestrator) independently reimplemented the same "1 token ≈ 4
// characters" estimate and the same "erase oldest non-system message until
// under budget" eviction loop, each with its own compaction-threshold
// constant. This header centralizes both so future changes to the heuristic
// or eviction strategy only need to be made once.
//
// Internal-only: consumed exclusively by orchestrator .cpp files, never
// exposed via include/qai_forge/.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <algorithm>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::ordered_json;

namespace qai_forge {
namespace utils {

/**
 * Estimate the token count of a rendered {messages, tools} payload using the
 * "1 token ≈ 4 characters" heuristic against each JSON value's serialized
 * (dump()) byte length. Matches the estimate every orchestrator already used
 * for context-eviction budgeting — not a precise tokenizer count.
 */
inline int estimateTokensApprox(const json& messages, const json& tools) {
    const std::size_t characters = messages.dump().size() + tools.dump().size();
    return static_cast<int>(characters / 4) + 1;
}

/**
 * Estimate the token count of a single string using the same "1 token ≈ 4
 * characters" heuristic.
 */
inline int estimateTokensApprox(const std::string& text) {
    return static_cast<int>(text.size() / 4) + 1;
}

/**
 * Evict the oldest non-system messages from `messages` (in place) until the
 * estimated token count of {messages, tools} is at or under
 * `max_context_length * compaction_threshold`, or fewer than 3 messages
 * remain (always keeps at least a system message, if present, plus one
 * other message). Returns the number of messages evicted.
 *
 * Shared by LiteRTLMOrchestrator and LlamaCppOrchestrator, whose eviction
 * loops were otherwise identical apart from their compaction_threshold
 * constant (0.75 vs 0.70).
 *
 * Performance note: the naive implementation of this loop re-dumps the
 * *entire* (shrinking) messages array via estimateTokensApprox() on every
 * iteration just to re-check the token count after removing one message —
 * O(n^2) in the number of evicted messages for a long conversation that
 * needs heavy eviction. This implementation instead tracks the running
 * serialized-character total incrementally: `tools` never changes during
 * eviction so its dumped size is computed once, and each removed message's
 * own (already-necessarily-dumped-once) size is subtracted from a running
 * total rather than re-summing everything that remains. This makes the
 * loop O(n) in the number of removed messages instead of O(n^2), while
 * producing the same eviction decisions as the token-count comparison
 * (subject to the same 1-byte-per-separator approximation inherent to
 * treating a JSON array's dump() as sum-of-elements plus punctuation).
 */
inline int applyFrontEviction(json& messages,
                              const json& tools,
                              int max_context_length,
                              float compaction_threshold) {
    if (max_context_length <= 0) {
        return 0;
    }

    const int limit = static_cast<int>(max_context_length * compaction_threshold);

    const std::size_t tools_chars = tools.dump().size();
    std::size_t total_chars = messages.dump().size() + tools_chars;

    auto tokensFromChars = [](std::size_t chars) {
        return static_cast<int>(chars / 4) + 1;
    };

    int evicted = 0;
    while (messages.size() > 2 && tokensFromChars(total_chars) > limit) {
        const bool has_system =
            messages.front().is_object() &&
            messages.front().value("role", "") == "system";
        const std::size_t index = has_system ? 1 : 0;
        if (index >= messages.size() - 1) {
            break;
        }

        // Removing one array element also drops its adjoining comma
        // separator (except for the single-element-remaining case already
        // excluded by the messages.size() > 2 guard above). Approximated as
        // 1 byte — consistent with the heuristic (not exact-tokenizer)
        // nature of this whole estimate.
        const std::size_t removed_chars = messages[index].dump().size() + 1;
        total_chars -= std::min(removed_chars, total_chars);

        messages.erase(index);
        ++evicted;
    }
    return evicted;
}

} // namespace utils
} // namespace qai_forge
