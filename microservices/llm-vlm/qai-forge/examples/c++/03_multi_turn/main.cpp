// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// Example 03: Multi-Turn Conversation
//
// Demonstrates caller-owned history with QaiForge-owned runtime memory.
//
// Key concepts:
//   - Sending the complete transcript on every request
//   - Passing explicit conversation and turn identifiers
//   - Releasing private runtime memory when the conversation ends
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/QaiForge.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/InternalDTOs.h"
#include <iostream>
#include <string>
#include <vector>

// Helper: send one turn and print the response
static std::string send_turn(const std::string& model_id,
                              const std::string& conversation_id,
                              const std::string& turn_id,
                              const std::string& parent_turn_id,
                              const std::vector<json>& messages) {
    CreateChatCompletionRequest request;
    request.model = model_id;
    request.stream = false;
    request.messages = json(messages);
    request.max_completion_tokens = 256;
    request.temperature = 0.7f;
    request.user = conversation_id;

    qai_forge::GenerateOptions options;
    options.response_id = turn_id;
    options.session_id = conversation_id;
    qai_forge::ConversationReference reference;
    reference.namespace_id = "qai_forge.example.multi_turn";
    reference.conversation_id = conversation_id;
    reference.turn_id = turn_id;
    if (parent_turn_id.empty()) {
        reference.parent_policy = qai_forge::ConversationParentPolicy::Root;
    } else {
        reference.parent_policy = qai_forge::ConversationParentPolicy::Explicit;
        reference.parent_turn_id = parent_turn_id;
    }
    options.conversation = std::move(reference);

    StandardResponse response =
        qai_forge::QaiForge::getInstance().generate(request, options);
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

    const std::string conversation_id = "example-03-session";
    qai_forge::QaiForge::getInstance().start();

    // ── Turn 1: Introduce a topic ──────────────────────────────────────────────
    std::cout << "Turn 1\n";
    std::cout << "User: My name is Alice and I'm learning about neural networks.\n";

    std::vector<json> messages_1 = {
        {{"role", "system"}, {"content", "You are a helpful AI tutor."}},
        {{"role", "user"},   {"content", "My name is Alice and I'm learning about neural networks."}}
    };

    try {
        std::string reply_1 = send_turn(
            model_id, conversation_id, "turn-1", "", messages_1);
        std::cout << "Assistant: " << reply_1 << "\n\n";

        // ── Turn 2: Follow-up question ─────────────────────────────────────────
        // The caller remains authoritative for the complete transcript.
        std::cout << "Turn 2\n";
        std::cout << "User: What is backpropagation?\n";

        std::vector<json> messages_2 = {
            {{"role", "system"}, {"content", "You are a helpful AI tutor."}},
            {{"role", "user"},   {"content", "My name is Alice and I'm learning about neural networks."}},
            {{"role", "assistant"}, {"content", reply_1}},
            {{"role", "user"},   {"content", "What is backpropagation?"}}
        };

        std::string reply_2 = send_turn(
            model_id, conversation_id, "turn-2", "turn-1", messages_2);
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

        std::string reply_3 = send_turn(
            model_id, conversation_id, "turn-3", "turn-2", messages_3);
        std::cout << "Assistant: " << reply_3 << "\n\n";

        qai_forge::QaiForge::getInstance().releaseConversation(
            "qai_forge.example.multi_turn", conversation_id);
        std::cout << "Runtime memory released.\n";

    } catch (const GenAIException& e) {
        std::cerr << "GenAI error (HTTP " << e.http_status << "): " << e.message << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    qai_forge::QaiForge::getInstance().shutdown();
    return 0;
}
