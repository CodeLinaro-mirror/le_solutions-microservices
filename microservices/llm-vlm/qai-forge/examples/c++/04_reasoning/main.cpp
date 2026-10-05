// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// Example 04: Reasoning Model with Thinking Tokens
//
// Demonstrates how qai-forge handles reasoning models (DeepSeek-R1, Qwen3,
// QwQ) that generate <think>...</think> blocks before their final answer.
//
// Key concepts:
//   - ReasoningRouter: routes <think> tokens to reasoning_content, enabled
//     automatically for models with supports_thinking=true in metadata.json
//   - StreamChunk.reasoning_content vs StreamChunk.content_delta (streaming)
//   - StandardResponse.reasoning_content (non-streaming)
//   - QaiForge::generate() / generateStream() facade (same API as 01/02/03)
//
// Model selection (checked in order):
//   1. argv[1]              e.g. ./04_reasoning qwen3-8b
//   2. QAI_FORGE_MODEL env var
//   3. Fallback default: "qwen3-8b"
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/QaiForge.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/InternalDTOs.h"
#include <iostream>
#include <string>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstdlib>
#include <algorithm>

namespace {

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
    // Default to a reasoning model — change to your deployed model ID
    const std::string model_id = resolveModelId(argc, argv, "qwen3-8b");
    const std::string problem  = (argc > 2) ? argv[2]
        : "A train travels 120 km in 2 hours. Another train travels 180 km in 3 hours. "
          "Which train is faster, and by how much?";

    std::cout << "=== qai-forge Example 04: Reasoning Model ===\n\n";
    std::cout << "Model:   " << model_id
               << " (override with argv[1] or QAI_FORGE_MODEL env var)\n";
    std::cout << "Problem: " << problem << "\n\n";

    qai_forge::QaiForge::getInstance().start();

    // Initialize model registry
    ModelConfigManager::getInstance().scanModelBundles();
    if (!ModelConfigManager::getInstance().validateModel(model_id)) {
        std::cerr << "ERROR: Model '" << model_id << "' not found.\n";
        qai_forge::QaiForge::getInstance().shutdown();
        return 1;
    }

    // Check if this model supports thinking
    bool supports_thinking = ModelConfigManager::getInstance().supportsThinking(model_id);
    std::cout << "Supports thinking: " << (supports_thinking ? "yes" : "no") << "\n\n";

    // ── Option A: Streaming (shows thinking tokens in real time) ──────────────
    std::cout << "─── Streaming with thinking tokens ─────────────────────\n";

    CreateChatCompletionRequest stream_request;
    stream_request.model = model_id;
    stream_request.stream = true;
    stream_request.messages = {
        {{"role", "system"}, {"content", "You are a precise mathematical reasoner."}},
        {{"role", "user"},   {"content", problem}}
    };
    stream_request.max_completion_tokens = 2048;
    stream_request.temperature = 0.6f;

    int thinking_tokens = 0;
    int answer_tokens = 0;
    bool in_thinking = false;
    auto start = std::chrono::steady_clock::now();

    std::mutex done_mutex;
    std::condition_variable done_cv;
    bool done = false;
    bool had_error = false;
    std::string error_message;

    qai_forge::StreamCallbacks callbacks;
    callbacks.onToken = [&](const StreamChunk& chunk) {
        if (chunk.reasoning_content.has_value()) {
            if (!in_thinking) {
                std::cout << "\033[2m[Thinking]\n";
                in_thinking = true;
            }
            std::cout << chunk.reasoning_content.value();
            std::cout.flush();
            thinking_tokens++;
        }

        if (chunk.content_delta.has_value()) {
            if (in_thinking) {
                std::cout << "\033[0m\n[Answer]\n";
                in_thinking = false;
            }
            std::cout << chunk.content_delta.value();
            std::cout.flush();
            answer_tokens++;
        }
    };
    callbacks.onComplete = [&](const StandardResponse&) {
        std::lock_guard<std::mutex> lock(done_mutex);
        done = true;
        done_cv.notify_one();
    };
    callbacks.onError = [&](const GenAIException& e) {
        std::lock_guard<std::mutex> lock(done_mutex);
        had_error = true;
        error_message = e.message;
        done = true;
        done_cv.notify_one();
    };
    callbacks.onCancelled = [&]() {
        std::lock_guard<std::mutex> lock(done_mutex);
        had_error = true;
        error_message = "Request cancelled";
        done = true;
        done_cv.notify_one();
    };

    try {
        qai_forge::QaiForge::getInstance().generateStream(stream_request, callbacks);
        std::unique_lock<std::mutex> lock(done_mutex);
        done_cv.wait(lock, [&] { return done; });
        if (in_thinking) std::cout << "\033[0m";
        if (had_error) {
            std::cerr << "\nGenAI error: " << error_message << "\n";
            qai_forge::QaiForge::getInstance().shutdown();
            return 1;
        }
    } catch (const GenAIException& e) {
        std::cerr << "\nGenAI error: " << e.message << "\n";
        qai_forge::QaiForge::getInstance().shutdown();
        return 1;
    }

    auto end = std::chrono::steady_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(end - start).count();

    std::cout << "\n────────────────────────────────────────────────────────\n\n";
    std::cout << "Thinking tokens: " << thinking_tokens << "\n";
    std::cout << "Answer tokens:   " << answer_tokens << "\n";
    std::cout << "Total time:      " << total_ms << " ms\n\n";

    // ── Option B: Non-streaming (returns full thinking + answer at once) ───────
    std::cout << "─── Non-streaming (full response) ──────────────────────\n";

    CreateChatCompletionRequest block_request;
    block_request.model = model_id;
    block_request.stream = false;
    block_request.messages = {
        {{"role", "system"}, {"content", "You are a precise mathematical reasoner."}},
        {{"role", "user"},   {"content", problem}}
    };
    block_request.max_completion_tokens = 2048;
    block_request.temperature = 0.6f;

    int exit_code = 0;
    try {
        StandardResponse response =
            qai_forge::QaiForge::getInstance().generate(block_request);

        if (response.reasoning_content.has_value() && !response.reasoning_content.value().empty()) {
            std::cout << "\n[Thinking]\n";
            // Print first 200 chars of thinking to keep output manageable
            const auto& thinking = response.reasoning_content.value();
            std::cout << thinking.substr(0, std::min(thinking.size(), size_t(200)));
            if (thinking.size() > 200) std::cout << "...(truncated)";
            std::cout << "\n";
        }

        std::cout << "\n[Answer]\n";
        std::cout << response.content.value_or("(no content)") << "\n";
        std::cout << "────────────────────────────────────────────────────────\n\n";

        std::cout << "Finish reason:     " << response.finish_reason << "\n";
        std::cout << "Completion tokens: " << response.completion_tokens << "\n";

    } catch (const GenAIException& e) {
        std::cerr << "GenAI error: " << e.message << "\n";
        exit_code = 1;
    }

    qai_forge::QaiForge::getInstance().shutdown();
    return exit_code;
}
