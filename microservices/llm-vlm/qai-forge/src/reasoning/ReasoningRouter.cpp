// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/reasoning/ReasoningRouter.h"
#include <iostream>

// ─────────────────────────────────────────────────────────────────────────────
// ReasoningBudgetTracker
// ─────────────────────────────────────────────────────────────────────────────
ReasoningBudgetTracker::ReasoningBudgetTracker(int budget_tokens)
    : budget_(budget_tokens), remaining_(budget_tokens) {
    if (budget_tokens == 0) {
        state_ = BudgetState::FORCING;
    }
}

void ReasoningBudgetTracker::activate() {
    if (budget_ == 0) {
        state_ = BudgetState::FORCING;
        return;
    }
    state_ = BudgetState::COUNTING;
    remaining_ = budget_;
    counted_ = 0;
}

void ReasoningBudgetTracker::deactivate() {
    state_ = BudgetState::DONE;
}

BudgetState ReasoningBudgetTracker::accept(const std::string& text_fragment) {
    if (state_ == BudgetState::COUNTING) {
        // Estimate tokens using the same heuristic as GenieOrchestrator (1 token ≈ 4 chars)
        int fragment_tokens = static_cast<int>(text_fragment.size() / 4);
        if (fragment_tokens < 1) fragment_tokens = 1;  // Minimum 1 token per fragment
        counted_ += fragment_tokens;
        if (budget_ >= 0) {
            remaining_ -= fragment_tokens;
            if (remaining_ <= 0) {
                state_ = BudgetState::FORCING;
            }
        }
    }
    return state_;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReasoningRouter
// ─────────────────────────────────────────────────────────────────────────────
ReasoningRouter::ReasoningRouter(const std::string& session_id,
                                  const std::string& model_id,
                                  const std::string& start_tag,
                                  const std::string& end_tag,
                                  int budget_tokens)
    : session_id_(session_id), model_id_(model_id),
      start_tag_(start_tag), end_tag_(end_tag),
      budget_tracker_(budget_tokens) {}

// ─────────────────────────────────────────────────────────────────────────────
// findPartialPrefix — find the longest suffix of `data` that is a prefix of `tag`
// Used to detect partial tags at stream boundaries (e.g. data ends with "<thi")
// ─────────────────────────────────────────────────────────────────────────────
std::optional<std::string> ReasoningRouter::findPartialPrefix(const std::string& data,
                                                               const std::string& tag) {
    for (size_t len = std::min(tag.size() - 1, data.size()); len > 0; --len) {
        if (data.compare(data.size() - len, len, tag, 0, len) == 0) {
            return tag.substr(0, len);
        }
    }
    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// route — main routing logic
// ─────────────────────────────────────────────────────────────────────────────
std::vector<StreamChunk> ReasoningRouter::route(const std::string& token_fragment) {
    std::vector<StreamChunk> results;

    // Prepend any carry buffer from previous fragment
    carry_buffer_ += token_fragment;

    while (!carry_buffer_.empty()) {
        if (state_ == ChannelState::OUTSIDE) {
            // ── Looking for start_tag ──────────────────────────────────────
            size_t idx = carry_buffer_.find(start_tag_);
            if (idx == std::string::npos) {
                // Check for partial start_tag at end of buffer
                auto partial = findPartialPrefix(carry_buffer_, start_tag_);
                if (partial.has_value()) {
                    // Emit safe portion, hold partial tag in carry buffer
                    std::string safe = carry_buffer_.substr(0, carry_buffer_.size() - partial.value().size());
                    if (!safe.empty()) {
                        StreamChunk chunk;
                        chunk.id = session_id_;
                        chunk.model = model_id_;
                        chunk.content_delta = safe;
                        full_answer_ += safe;
                        results.push_back(chunk);
                    }
                    carry_buffer_ = partial.value();
                } else {
                    // No start_tag found — emit everything as answer content
                    StreamChunk chunk;
                    chunk.id = session_id_;
                    chunk.model = model_id_;
                    chunk.content_delta = carry_buffer_;
                    full_answer_ += carry_buffer_;
                    results.push_back(chunk);
                    carry_buffer_.clear();
                }
                break;
            }

            // Emit content before the start_tag
            if (idx > 0) {
                std::string before = carry_buffer_.substr(0, idx);
                StreamChunk chunk;
                chunk.id = session_id_;
                chunk.model = model_id_;
                chunk.content_delta = before;
                full_answer_ += before;
                results.push_back(chunk);
            }

            // Consume the start_tag and switch to INSIDE
            carry_buffer_ = carry_buffer_.substr(idx + start_tag_.size());
            state_ = ChannelState::INSIDE;

            // Activate budget tracker
            if (budget_tracker_.state() == BudgetState::IDLE) {
                budget_tracker_.activate();
            }

        } else {
            // ── Inside thinking block — looking for end_tag ────────────────
            size_t idx = carry_buffer_.find(end_tag_);
            if (idx == std::string::npos) {
                // Check for partial end_tag at end of buffer
                auto partial = findPartialPrefix(carry_buffer_, end_tag_);
                if (partial.has_value()) {
                    std::string safe = carry_buffer_.substr(0, carry_buffer_.size() - partial.value().size());
                    if (!safe.empty()) {
                        emitThinkingChunk(safe, results);
                    }
                    carry_buffer_ = partial.value();
                } else {
                    emitThinkingChunk(carry_buffer_, results);
                    carry_buffer_.clear();
                }
                break;
            }

            // Emit thinking content before end_tag
            if (idx > 0) {
                std::string thinking = carry_buffer_.substr(0, idx);
                emitThinkingChunk(thinking, results);
            }

            // Consume end_tag and switch back to OUTSIDE
            carry_buffer_ = carry_buffer_.substr(idx + end_tag_.size());
            state_ = ChannelState::OUTSIDE;
            budget_tracker_.deactivate();
        }
    }

    return results;
}

void ReasoningRouter::emitThinkingChunk(const std::string& text,
                                         std::vector<StreamChunk>& results) {
    if (text.empty()) return;

    // Check budget
    BudgetState budget_state = budget_tracker_.accept(text);

    if (budget_state == BudgetState::FORCING && !budget_forced_) {
        // Budget exhausted — inject truncation message and force exit
        budget_forced_ = true;
        std::string truncation_msg = "\n[Thinking truncated]\n";
        full_thinking_ += truncation_msg;

        StreamChunk truncation_chunk;
        truncation_chunk.id = session_id_;
        truncation_chunk.model = model_id_;
        truncation_chunk.reasoning_content = truncation_msg;
        results.push_back(truncation_chunk);

        // Redirect the fragment that tripped the budget into the answer channel
        // instead of discarding it. In blocking mode, route() is called once with
        // the entire remaining response as a single fragment, so dropping it here
        // would leave the answer empty.
        StreamChunk chunk;
        chunk.id = session_id_;
        chunk.model = model_id_;
        chunk.content_delta = text;
        full_answer_ += text;
        results.push_back(chunk);

        // Force router out of thinking channel
        state_ = ChannelState::OUTSIDE;
        return;
    }

    if (budget_forced_) {
        // After budget forced, treat remaining thinking tokens as answer
        StreamChunk chunk;
        chunk.id = session_id_;
        chunk.model = model_id_;
        chunk.content_delta = text;
        full_answer_ += text;
        results.push_back(chunk);
        return;
    }

    // Normal thinking token
    full_thinking_ += text;
    StreamChunk chunk;
    chunk.id = session_id_;
    chunk.model = model_id_;
    chunk.reasoning_content = text;
    results.push_back(chunk);
}
