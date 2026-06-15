// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// SummarizationMiddleware — P4 Implementation
//
// Performs context summarization when the projected token count exceeds
// the threshold. Calls the InferenceWorkerManager to generate a summary,
// updates session.summary_content, and resets the worker's KV cache.
//
// Context Compaction formula (Section 6 of architecture design):
//   Prompt = [system_prompt] + [summary] + [post-summary messages] + [user_message]
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/orchestration/SummarizationMiddleware.h"
#include "qai_forge/backend/IGenerativeBackend.h"
#include "qai_forge/worker/InferenceProtocol.h"
#include "qai_forge/utils/Logger.h"
#include <sstream>

// ─────────────────────────────────────────────────────────────────────────────
// estimateTokenCount — rough heuristic (1 token ≈ 4 chars)
// P7 TokenCounter will replace this with a proper implementation.
// ─────────────────────────────────────────────────────────────────────────────
int SummarizationMiddleware::estimateTokenCount(const ConversationSession& session,
                                                 const CreateChatCompletionRequest& request) {
    int total = 0;

    // Existing session history
    for (const auto& msg : session.messages) {
        total += static_cast<int>(msg.dump().size() / 4);
    }

    // Summary (if any)
    total += static_cast<int>(session.summary_content.size() / 4);

    // New messages in the request
    for (const auto& msg : request.messages) {
        total += static_cast<int>(msg.dump().size() / 4);
    }

    return total;
}

// ─────────────────────────────────────────────────────────────────────────────
// buildSummarizationPrompt — construct the prompt sent to the LLM for summarization
// ─────────────────────────────────────────────────────────────────────────────
std::string SummarizationMiddleware::buildSummarizationPrompt(const ConversationSession& session,
                                                               const std::string& model_id) {
    std::ostringstream prompt;

    prompt << "<|system|>\n"
           << "You are a helpful assistant. Your task is to create a concise summary "
           << "of the conversation below. The summary should capture the key points, "
           << "decisions, and context needed to continue the conversation. "
           << "Be brief but comprehensive.\n\n";

    // Include previous summary if available
    if (!session.summary_content.empty()) {
        prompt << "Previous summary:\n" << session.summary_content << "\n\n";
    }

    // Include clean message history (strips private "_*" keys)
    prompt << "Conversation to summarize:\n";
    auto clean_messages = session.getCleanMessages();
    for (const auto& msg : clean_messages) {
        std::string role = msg.value("role", "");
        std::string content = msg.value("content", "");
        if (role == "system") continue; // Skip system messages in the summary input
        if (role == "user") {
            prompt << "User: " << content << "\n";
        } else if (role == "assistant") {
            prompt << "Assistant: " << content << "\n";
        }
    }

    prompt << "\n<|user|>\nPlease provide a concise summary of the above conversation.\n"
           << "<|assistant|>\n";

    return prompt.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// checkAndSummarize — main entry point
// ─────────────────────────────────────────────────────────────────────────────
bool SummarizationMiddleware::checkAndSummarize(ConversationSession& session,
                                                 const CreateChatCompletionRequest& request,
                                                 IGenerativeBackend& backend,
                                                 int context_size,
                                                 float threshold) {
    // Need at least 2 events to summarize
    if (session.messages.size() < 4) return false;

    int estimated_tokens = estimateTokenCount(session, request);
    int threshold_tokens = static_cast<int>(static_cast<float>(context_size) * threshold);

    if (estimated_tokens < threshold_tokens) return false;

    LOG_INFO("[SummarizationMiddleware] Threshold reached for session "
             << session.session_id << " (estimated " << estimated_tokens
             << " tokens, threshold " << threshold_tokens << "). Generating summary...");

    // Build summarization prompt
    std::string summary_prompt = buildSummarizationPrompt(session, request.model);

    // Generate summary via IGenerativeBackend::generate()
    // use_reasoning=false: we never want thinking tokens in a summary
    std::string summary_text;
    bool had_error = false;
    std::string error_msg;

    static int summary_counter = 0;
    std::string event_id = "summary-" + std::to_string(++summary_counter);

    backend.generate(
        event_id,
        summary_prompt,
        false,  // non-streaming — accumulate full summary
        512,    // max summary tokens
        0.3f,   // low temperature for deterministic summary
        1.0f,   // top_p
        40,     // top_k
        0.0f,   // presence_penalty
        0.0f,   // frequency_penalty
        false,  // use_reasoning=false — no thinking tokens in summary
        [&summary_text](const IPCTokenEvent& token) {
            summary_text += token.content;
        },
        [](const IPCDoneEvent& /*done*/) {
            // Summary complete
        },
        [&had_error, &error_msg](const IPCErrorEvent& err) {
            had_error = true;
            error_msg = err.message;
        }
    );

    if (had_error || summary_text.empty()) {
        LOG_ERROR("[SummarizationMiddleware] Failed to generate summary: "
                  << (had_error ? error_msg : "empty response"));
        return false;
    }

    // Update session with the new summary
    session.summary_content = summary_text;
    session.summary_token_count = static_cast<int>(summary_text.size() / 4);

    LOG_INFO("[SummarizationMiddleware] Summary generated ("
             << session.summary_token_count << " tokens). "
             << "Calling backend.onContextCompacted() to reset KV cache.");

    // Notify the backend that context has been compacted.
    // GenIEBackend: calls sendReset() to clear the KV cache so the next turn
    //               starts fresh with the compacted context.
    // OnnxRTBackend (future): no-op — recomputes full context each turn anyway.
    backend.onContextCompacted();

    return true;
}
