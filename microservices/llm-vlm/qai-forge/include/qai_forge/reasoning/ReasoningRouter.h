// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/InternalDTOs.h"
#include <string>
#include <vector>
#include <optional>

// ─────────────────────────────────────────────────────────────────────────────
// ReasoningRouter — Layer 2 Stream Processor (Section 3.F of architecture design)
//
// Sits between the InferenceWorkerManager token stream and Layer 1.
// Routes raw token fragments to the correct DTO field:
//   - Inside <think>...</think>  → StreamChunk(reasoning_content=token)
//   - Outside thinking block     → StreamChunk(content_delta=token)
//
// Correctly handles partial tags at stream boundaries (e.g., one chunk ends
// with "<thi", next starts with "nk>") via an internal carry buffer.
//
// Used when bypass_think_filter=true (reasoning models like DeepSeek-R1, Qwen3).
// When bypass_think_filter=false, this router is not used — tokens go directly
// to content_delta.
// ─────────────────────────────────────────────────────────────────────────────

enum class ChannelState {
    OUTSIDE,  // Currently in the answer channel
    INSIDE    // Currently inside a <think>...</think> block
};

enum class BudgetState {
    IDLE,      // Not yet activated
    COUNTING,  // Counting thinking tokens
    FORCING,   // Budget exhausted — force exit from thinking
    DONE       // Natural end of thinking block
};

// ─────────────────────────────────────────────────────────────────────────────
// ReasoningBudgetTracker — enforces max thinking token budget
// ─────────────────────────────────────────────────────────────────────────────
class ReasoningBudgetTracker {
public:
    /**
     * @param budget_tokens  Max thinking tokens. -1 = unlimited, 0 = suppress immediately.
     */
    explicit ReasoningBudgetTracker(int budget_tokens);

    void activate();
    void deactivate();
    BudgetState accept(const std::string& text_fragment);

    bool isExhausted() const { return state_ == BudgetState::FORCING; }
    int tokensCounted() const { return counted_; }
    BudgetState state() const { return state_; }

private:
    int budget_;
    int remaining_;
    int counted_ = 0;
    BudgetState state_ = BudgetState::IDLE;
};

// ─────────────────────────────────────────────────────────────────────────────
// ReasoningRouter — routes raw token stream to typed StreamChunk DTOs
// ─────────────────────────────────────────────────────────────────────────────
class ReasoningRouter {
public:
    /**
     * @param session_id     Used to populate StreamChunk.id
     * @param model_id       Used to populate StreamChunk.model
     * @param start_tag      e.g. "<think>"
     * @param end_tag        e.g. "</think>"
     * @param budget_tokens  Max thinking tokens (-1 = unlimited)
     */
    ReasoningRouter(const std::string& session_id,
                    const std::string& model_id,
                    const std::string& start_tag = "<think>",
                    const std::string& end_tag = "</think>",
                    int budget_tokens = -1);

    /**
     * Process a raw token fragment from the inference worker.
     * Returns zero or more StreamChunk DTOs to be sent to Layer 1.
     *
     * Handles partial tag buffering: if a fragment ends mid-tag (e.g. "<thi"),
     * the partial tag is held in the carry buffer until the next fragment arrives.
     */
    std::vector<StreamChunk> route(const std::string& token_fragment);

    /**
     * Returns true if the router is currently inside a thinking block.
     */
    bool isInsideThinking() const { return state_ == ChannelState::INSIDE; }

    /**
     * Force the router out of the thinking channel (used when budget is exhausted).
     */
    void forceOutside() { state_ = ChannelState::OUTSIDE; }

    /**
     * Returns the accumulated full thinking content (for non-streaming responses).
     */
    const std::string& getThinkingContent() const { return full_thinking_; }

    /**
     * Returns the accumulated full answer content (for non-streaming responses).
     */
    const std::string& getAnswerContent() const { return full_answer_; }

    /**
     * Returns the number of thinking token fragments counted.
     */
    int getThinkingTokenCount() const { return budget_tracker_.tokensCounted(); }

private:
    std::string session_id_;
    std::string model_id_;
    std::string start_tag_;
    std::string end_tag_;

    ChannelState state_ = ChannelState::OUTSIDE;
    std::string carry_buffer_;

    ReasoningBudgetTracker budget_tracker_;
    bool budget_forced_ = false;

    std::string full_thinking_;
    std::string full_answer_;

    // Find the longest suffix of `data` that is a prefix of `tag`
    static std::optional<std::string> findPartialPrefix(const std::string& data,
                                                          const std::string& tag);

    // Emit a thinking-channel chunk, enforcing the budget tracker.
    // Appends zero or more StreamChunks to `results`.
    void emitThinkingChunk(const std::string& text, std::vector<StreamChunk>& results);
};
