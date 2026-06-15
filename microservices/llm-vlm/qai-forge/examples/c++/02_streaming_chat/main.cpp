// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// Example 02: Streaming Chat Completion
//
// Demonstrates streaming token delivery via the StreamCallback interface.
// Tokens are printed to stdout as they arrive from the inference worker,
// giving the user a real-time "typewriter" effect.
//
// Key concepts:
//   - ChatOrchestrator::handleStreaming() with a StreamCallback
//   - StreamChunk DTO: content_delta, reasoning_content, finish_reason
//   - Measuring Time-To-First-Token (TTFT) and tokens/second
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/orchestration/ChatOrchestrator.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/InternalDTOs.h"
#include <iostream>
#include <string>
#include <chrono>
#include <atomic>

int main(int argc, char* argv[]) {
    const std::string model_id = (argc > 1) ? argv[1] : "qwen2.5-7b";
    const std::string user_message = (argc > 2) ? argv[2]
        : "Explain the difference between LLMs and VLMs in 3 sentences.";

    std::cout << "=== qai-forge Example 02: Streaming Chat ===\n\n";
    std::cout << "Model:   " << model_id << "\n";
    std::cout << "Message: " << user_message << "\n\n";

    // Initialize model registry
    ModelConfigManager::getInstance().scanModelBundles();
    if (!ModelConfigManager::getInstance().validateModel(model_id)) {
        std::cerr << "ERROR: Model '" << model_id << "' not found.\n";
        return 1;
    }

    // Build request
    CreateChatCompletionRequest request;
    request.model = model_id;
    request.stream = true;
    request.messages = {
        {{"role", "system"}, {"content", "You are a helpful assistant."}},
        {{"role", "user"},   {"content", user_message}}
    };
    request.max_completion_tokens = 512;
    request.temperature = 0.7f;

    // ── Streaming metrics ──────────────────────────────────────────────────────
    auto start_time = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point first_token_time;
    bool first_token_received = false;
    int token_count = 0;
    std::string finish_reason;

    std::cout << "─── Streaming Response ─────────────────────────────────\n";

    try {
        // ── handleStreaming() with a StreamCallback ────────────────────────────
        // The callback is called synchronously for each StreamChunk.
        // StreamChunk fields:
        //   - role:              set on the first chunk only
        //   - content_delta:     a token fragment (answer text)
        //   - reasoning_content: a thinking token (reasoning models only)
        //   - finish_reason:     set on the last chunk ("stop", "length", etc.)
        ChatOrchestrator::getInstance().handleStreaming(request,
            [&](const StreamChunk& chunk) {
                // Track TTFT on first content token
                if (!first_token_received &&
                    (chunk.content_delta.has_value() || chunk.reasoning_content.has_value())) {
                    first_token_time = std::chrono::steady_clock::now();
                    first_token_received = true;
                }

                // Print reasoning content (thinking models)
                if (chunk.reasoning_content.has_value()) {
                    // Print thinking tokens in dim color if terminal supports it
                    std::cout << "\033[2m" << chunk.reasoning_content.value() << "\033[0m";
                    std::cout.flush();
                    token_count++;
                }

                // Print answer content
                if (chunk.content_delta.has_value()) {
                    std::cout << chunk.content_delta.value();
                    std::cout.flush();
                    token_count++;
                }

                // Capture finish reason
                if (chunk.finish_reason.has_value()) {
                    finish_reason = chunk.finish_reason.value();
                }
            }
        );

    } catch (const GenAIException& e) {
        std::cerr << "\nGenAI error (HTTP " << e.http_status << "): " << e.message << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        return 1;
    }

    auto end_time = std::chrono::steady_clock::now();
    std::cout << "\n────────────────────────────────────────────────────────\n\n";

    // ── Print metrics ──────────────────────────────────────────────────────────
    double total_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    double ttft_ms  = first_token_received
        ? std::chrono::duration<double, std::milli>(first_token_time - start_time).count()
        : 0.0;
    double tps = (total_ms > 0) ? (token_count * 1000.0 / total_ms) : 0.0;

    std::cout << "Finish reason:       " << finish_reason << "\n";
    std::cout << "Tokens generated:    " << token_count << "\n";
    std::cout << "Time to first token: " << ttft_ms << " ms\n";
    std::cout << "Total time:          " << total_ms << " ms\n";
    std::cout << "Tokens/second:       " << tps << "\n";

    return 0;
}
