// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// Example 01: Simple Blocking Chat Completion
//
// Demonstrates the most basic usage of the qai-forge SDK:
//   1. Start the QaiForge facade (loads plugins, starts the scheduler)
//   2. Scan the model registry
//   3. Build a CreateChatCompletionRequest
//   4. Call QaiForge::generate()
//   5. Print the response
//
// Model selection (checked in order):
//   1. argv[1]              e.g. ./01_simple_chat qwen2.5-7b
//   2. QAI_FORGE_MODEL env var
//   3. Fallback default: "qwen2.5-7b"
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/QaiForge.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/InternalDTOs.h"
#include <iostream>
#include <string>
#include <cstdlib>

namespace {

// Resolve the model id to use: CLI arg > env var > fallback default.
std::string resolveModelId(int argc, char* argv[], const std::string& fallback) {
    if (argc > 1 && argv[1][0] != '\0') {
        return argv[1];
    }
    if (const char* env = std::getenv("QAI_FORGE_MODEL")) {
        if (env[0] != '\0') return env;
    }
    return fallback;
}

} // namespace

int main(int argc, char* argv[]) {
    // ── Configuration ─────────────────────────────────────────────────────────
    const std::string model_id = resolveModelId(argc, argv, "qwen2.5-7b");
    const std::string user_message = (argc > 2) ? argv[2] : "What is the capital of France?";

    std::cout << "=== qai-forge Example 01: Simple Blocking Chat ===\n\n";
    std::cout << "Model:   " << model_id
               << " (override with argv[1] or QAI_FORGE_MODEL env var)\n";
    std::cout << "Message: " << user_message << "\n\n";

    // ── Step 1: Start the facade ───────────────────────────────────────────────
    // QaiForge::start() loads backend/orchestrator plugins and starts the
    // internal scheduler. Must be called once before any generate() call.
    qai_forge::QaiForge::getInstance().start();

    // ── Step 2: Initialize model registry ─────────────────────────────────────
    // ModelConfigManager scans the models directory (GENAI_MODELS_DIR env var,
    // or the built-in default) for model bundles.
    std::cout << "Scanning model bundles...\n";
    ModelConfigManager::getInstance().scanModelBundles();

    // Verify the model exists
    if (!ModelConfigManager::getInstance().validateModel(model_id)) {
        std::cerr << "ERROR: Model '" << model_id << "' not found.\n";
        std::cerr << "Available models:\n";
        for (const auto& mc : ModelConfigManager::getInstance().getAvailableModels()) {
            std::cerr << "  - " << mc.id << " (" << mc.display_name << ")\n";
        }
        qai_forge::QaiForge::getInstance().shutdown();
        return 1;
    }

    // ── Step 3: Build the request ──────────────────────────────────────────────
    // CreateChatCompletionRequest is the transport-agnostic DTO that QaiForge
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

    // ── Step 4: Run inference ───────────────────────────────────────────────────
    // QaiForge::generate() runs the full pipeline: validate → schedule →
    // inference → return StandardResponse.
    std::cout << "Running inference...\n\n";

    int exit_code = 0;
    try {
        StandardResponse response =
            qai_forge::QaiForge::getInstance().generate(request);

        // ── Step 5: Print the response ─────────────────────────────────────────
        std::cout << "─── Response ───────────────────────────────────────────\n";
        std::cout << response.content.value_or("(no content)") << "\n";
        std::cout << "────────────────────────────────────────────────────────\n\n";

        std::cout << "Response ID:       " << response.id << "\n";
        std::cout << "Finish reason:     " << response.finish_reason << "\n";
        std::cout << "Prompt tokens:     " << response.prompt_tokens << "\n";
        std::cout << "Completion tokens: " << response.completion_tokens << "\n";
        std::cout << "Total tokens:      " << response.total_tokens << "\n";

    } catch (const GenAIException& e) {
        std::cerr << "GenAI error (HTTP " << e.http_status << "): " << e.message << "\n";
        exit_code = 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        exit_code = 1;
    }

    qai_forge::QaiForge::getInstance().shutdown();
    return exit_code;
}
