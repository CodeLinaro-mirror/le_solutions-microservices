// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// Example 01: Simple Blocking Chat Completion
//
// Demonstrates the most basic usage of the qai-forge SDK:
//   1. Initialize the model registry
//   2. Build a CreateChatCompletionRequest
//   3. Call ChatOrchestrator::handleBlocking()
//   4. Print the response
//
// This is the equivalent of:
//   curl -X POST http://localhost:9001/v1/chat/completions \
//     -d '{"model":"qwen2.5-7b","messages":[{"role":"user","content":"Hello!"}]}'
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/orchestration/ChatOrchestrator.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/InternalDTOs.h"
#include <iostream>
#include <string>
#include <cstdlib>

int main(int argc, char* argv[]) {
    // ── Configuration ─────────────────────────────────────────────────────────
    const std::string model_id = (argc > 1) ? argv[1] : "qwen2.5-7b";
    const std::string user_message = (argc > 2) ? argv[2] : "What is the capital of France?";

    std::cout << "=== qai-forge Example 01: Simple Blocking Chat ===\n\n";
    std::cout << "Model:   " << model_id << "\n";
    std::cout << "Message: " << user_message << "\n\n";

    // ── Step 1: Initialize model registry ─────────────────────────────────────
    // ModelConfigManager scans /mnt/work/models for model bundles.
    // Must be called before any inference requests.
    std::cout << "Scanning model bundles...\n";
    ModelConfigManager::getInstance().scanModelBundles();

    // Verify the model exists
    if (!ModelConfigManager::getInstance().validateModel(model_id)) {
        std::cerr << "ERROR: Model '" << model_id << "' not found.\n";
        std::cerr << "Available models:\n";
        for (const auto& mc : ModelConfigManager::getInstance().getAvailableModels()) {
            std::cerr << "  - " << mc.id << " (" << mc.display_name << ")\n";
        }
        return 1;
    }

    // ── Step 2: Build the request ──────────────────────────────────────────────
    // CreateChatCompletionRequest is the transport-agnostic DTO that Layer 2
    // accepts. It mirrors the OpenAI Chat Completions API request body.
    CreateChatCompletionRequest request;
    request.model = model_id;
    request.stream = false;  // blocking mode
    request.messages = {
        {{"role", "system"}, {"content", "You are a helpful assistant."}},
        {{"role", "user"},   {"content", user_message}}
    };
    request.max_completion_tokens = 512;
    request.temperature = 0.7f;
    request.top_p = 0.9f;

    // ── Step 3: Run inference ──────────────────────────────────────────────────
    // ChatOrchestrator::handleBlocking() runs the full pipeline:
    //   validate → session → middleware → inference → return StandardResponse
    std::cout << "Running inference...\n\n";

    try {
        StandardResponse response = ChatOrchestrator::getInstance().handleBlocking(request);

        // ── Step 4: Print the response ─────────────────────────────────────────
        std::cout << "─── Response ───────────────────────────────────────────\n";
        std::cout << response.content.value_or("(no content)") << "\n";
        std::cout << "────────────────────────────────────────────────────────\n\n";

        std::cout << "Session ID:        " << response.id << "\n";
        std::cout << "Finish reason:     " << response.finish_reason << "\n";
        std::cout << "Prompt tokens:     " << response.prompt_tokens << "\n";
        std::cout << "Completion tokens: " << response.completion_tokens << "\n";
        std::cout << "Total tokens:      " << response.total_tokens << "\n";

    } catch (const GenAIException& e) {
        std::cerr << "GenAI error (HTTP " << e.http_status << "): " << e.message << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
