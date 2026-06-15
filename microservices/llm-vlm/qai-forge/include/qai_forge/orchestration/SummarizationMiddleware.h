// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/session/ConversationSession.h"
#include "qai_forge/InternalDTOs.h"
#include "qai_forge/backend/IGenerativeBackend.h"
#include <string>
#include <functional>

// ─────────────────────────────────────────────────────────────────────────────
// SummarizationMiddleware — P4 (Section 3.D of architecture design)
//
// Checks if the projected token count for the current turn exceeds the
// summarization threshold (default: 70% of context window). If so, it:
//   1. Builds a summarization prompt from the session history.
//   2. Calls IGenerativeBackend::generate() to produce a summary.
//   3. Updates session.summary_content with the generated summary.
//   4. Calls IGenerativeBackend::onContextCompacted() so the backend can
//      reset its KV cache (GenIE: sendReset; OnnxRT: no-op).
//
// Context Compaction formula (Section 6 of architecture design):
//   Prompt = [system_prompt] + [summary] + [post-summary messages] + [user_message]
//
// This is a stateless utility class — it does not hold any session state.
//
// Design change (docs/genai-backend-decoupling.md):
//   Previously took InferenceWorkerManager& and called worker.sendReset() directly.
//   Now takes IGenerativeBackend& and calls backend.onContextCompacted() — the
//   backend decides how to reset (GenIE: sendReset; future backends: no-op).
// ─────────────────────────────────────────────────────────────────────────────

class SummarizationMiddleware {
public:
    /**
     * Check if summarization is needed and perform it if so.
     *
     * @param session         The current conversation session
     * @param request         The incoming request (used for model_id and context size)
     * @param backend         The active generative backend (used for inference + reset)
     * @param context_size    The model's context window size in tokens
     * @param threshold       Fraction of context_size that triggers summarization (default 0.7)
     *
     * @return true if summarization was performed, false if not needed
     */
    static bool checkAndSummarize(ConversationSession& session,
                                   const CreateChatCompletionRequest& request,
                                   IGenerativeBackend& backend,
                                   int context_size,
                                   float threshold = 0.7f);

    /**
     * Estimate the token count for a set of messages.
     * Uses a rough heuristic: 1 token ≈ 4 characters.
     * (P7 TokenCounter will replace this with a proper implementation.)
     */
    static int estimateTokenCount(const ConversationSession& session,
                                   const CreateChatCompletionRequest& request);

    /**
     * Build the summarization prompt from the session history.
     * Asks the model to produce a concise summary of the conversation so far.
     */
    static std::string buildSummarizationPrompt(const ConversationSession& session,
                                                  const std::string& model_id);
};
