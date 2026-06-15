// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// Example 03: Multi-Turn Conversation
//
// Demonstrates persistent session management across multiple turns.
// The SDK maintains conversation history automatically — each subsequent
// request includes the full history via the session's message store.
//
// Key concepts:
//   - Using the `user` field to pin a stable session ID
//   - Session persistence: history is maintained between calls
//   - DraftTurn: new messages are staged and committed atomically
//   - Context compaction: SummarizationMiddleware triggers automatically
//     when the context window fills up
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/orchestration/ChatOrchestrator.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/session/SessionManager.h"
#include "qai_forge/InternalDTOs.h"
#include <iostream>
#include <string>
#include <vector>

// Helper: send one turn and print the response
static std::string send_turn(const std::string& model_id,
                              const std::string& session_id,
                              const std::vector<json>& messages) {
    CreateChatCompletionRequest request;
    request.model = model_id;
    request.stream = false;
    request.messages = json(messages);
    request.max_completion_tokens = 256;
    request.temperature = 0.7f;
    // Pin the session ID via the `user` field.
    // The orchestrator uses this as the stable session key.
    request.user = session_id;

    StandardResponse response = ChatOrchestrator::getInstance().handleBlocking(request);
    return response.content.value_or("");
}

int main(int argc, char* argv[]) {
    const std::string model_id = (argc > 1) ? argv[1] : "qwen2.5-7b";

    std::cout << "=== qai-forge Example 03: Multi-Turn Conversation ===\n\n";
    std::cout << "Model: " << model_id << "\n\n";

    // Initialize model registry
    ModelConfigManager::getInstance().scanModelBundles();
    if (!ModelConfigManager::getInstance().validateModel(model_id)) {
        std::cerr << "ERROR: Model '" << model_id << "' not found.\n";
        return 1;
    }

    // ── Assign a stable session ID ─────────────────────────────────────────────
    // Using a fixed session ID means all turns in this program share the same
    // ConversationSession. The session persists in memory for the lifetime of
    // the process (or until deleteSession() is called).
    const std::string session_id = "example-03-session";

    // ── Turn 1: Introduce a topic ──────────────────────────────────────────────
    std::cout << "Turn 1\n";
    std::cout << "User: My name is Alice and I'm learning about neural networks.\n";

    std::vector<json> messages_1 = {
        {{"role", "system"}, {"content", "You are a helpful AI tutor."}},
        {{"role", "user"},   {"content", "My name is Alice and I'm learning about neural networks."}}
    };

    try {
        std::string reply_1 = send_turn(model_id, session_id, messages_1);
        std::cout << "Assistant: " << reply_1 << "\n\n";

        // ── Turn 2: Follow-up question ─────────────────────────────────────────
        // The SDK automatically includes the previous turn in the context.
        // We only need to send the new user message — the session history
        // is managed internally by ConversationSession.
        std::cout << "Turn 2\n";
        std::cout << "User: What is backpropagation?\n";

        std::vector<json> messages_2 = {
            {{"role", "system"}, {"content", "You are a helpful AI tutor."}},
            {{"role", "user"},   {"content", "My name is Alice and I'm learning about neural networks."}},
            {{"role", "assistant"}, {"content", reply_1}},
            {{"role", "user"},   {"content", "What is backpropagation?"}}
        };

        std::string reply_2 = send_turn(model_id, session_id, messages_2);
        std::cout << "Assistant: " << reply_2 << "\n\n";

        // ── Turn 3: Test memory ────────────────────────────────────────────────
        std::cout << "Turn 3\n";
        std::cout << "User: Do you remember my name?\n";

        std::vector<json> messages_3 = {
            {{"role", "system"}, {"content", "You are a helpful AI tutor."}},
            {{"role", "user"},   {"content", "My name is Alice and I'm learning about neural networks."}},
            {{"role", "assistant"}, {"content", reply_1}},
            {{"role", "user"},   {"content", "What is backpropagation?"}},
            {{"role", "assistant"}, {"content", reply_2}},
            {{"role", "user"},   {"content", "Do you remember my name?"}}
        };

        std::string reply_3 = send_turn(model_id, session_id, messages_3);
        std::cout << "Assistant: " << reply_3 << "\n\n";

        // ── Session info ───────────────────────────────────────────────────────
        auto session = SessionManager::getInstance().getSession(session_id);
        if (session) {
            std::cout << "─── Session Info ───────────────────────────────────────\n";
            std::cout << "Session ID:    " << session->session_id << "\n";
            std::cout << "Messages:      " << session->messages.size() << "\n";
            std::cout << "Model:         " << session->current_model_id << "\n";
            if (!session->summary_content.empty()) {
                std::cout << "Summary:       " << session->summary_content.substr(0, 80) << "...\n";
            }
        }

        // ── Clean up ───────────────────────────────────────────────────────────
        // Delete the session to free memory. In a server context, sessions
        // persist until explicitly deleted or the server restarts.
        SessionManager::getInstance().deleteSession(session_id);
        std::cout << "\nSession deleted.\n";

    } catch (const GenAIException& e) {
        std::cerr << "GenAI error (HTTP " << e.http_status << "): " << e.message << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
