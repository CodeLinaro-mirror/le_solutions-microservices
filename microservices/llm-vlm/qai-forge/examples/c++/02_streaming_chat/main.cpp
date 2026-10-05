// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// Example 02: Streaming Chat Completion
//
// Demonstrates streaming token delivery via qai_forge::QaiForge::generateStream()
// and the qai_forge::StreamCallbacks interface. Tokens are printed to stdout as
// they arrive from the inference worker, giving the user a real-time
// "typewriter" effect.
//
// Key concepts:
//   - QaiForge::generateStream() with StreamCallbacks (onToken/onComplete/
//     onError/onCancelled)
//   - Callbacks run on a background thread — block the main thread with a
//     condition variable until onComplete/onError/onCancelled fires
//   - StreamChunk DTO: content_delta, reasoning_content, finish_reason
//   - Measuring Time-To-First-Token (TTFT) and tokens/second
//
// Model selection (checked in order):
//   1. argv[1]              e.g. ./02_streaming_chat qwen2.5-7b
//   2. QAI_FORGE_MODEL env var
//   3. Fallback default: "qwen2.5-7b"
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
    const std::string model_id = resolveModelId(argc, argv, "qwen2.5-7b");
    const std::string user_message = (argc > 2) ? argv[2]
        : "Explain the difference between LLMs and VLMs in 3 sentences.";

    std::cout << "=== qai-forge Example 02: Streaming Chat ===\n\n";
    std::cout << "Model:   " << model_id
               << " (override with argv[1] or QAI_FORGE_MODEL env var)\n";
    std::cout << "Message: " << user_message << "\n\n";

    qai_forge::QaiForge::getInstance().start();

    // Initialize model registry
    ModelConfigManager::getInstance().scanModelBundles();
    if (!ModelConfigManager::getInstance().validateModel(model_id)) {
        std::cerr << "ERROR: Model '" << model_id << "' not found.\n";
        qai_forge::QaiForge::getInstance().shutdown();
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

    // ── Block the main thread until the stream completes ───────────────────────
    // QaiForge::generateStream() returns immediately; completion is signaled
    // via the onComplete/onError/onCancelled callbacks on a background thread.
    std::mutex done_mutex;
    std::condition_variable done_cv;
    bool done = false;
    bool had_error = false;
    std::string error_message;
    int error_status = 0;

    qai_forge::StreamCallbacks callbacks;
    callbacks.onToken = [&](const StreamChunk& chunk) {
        if (!first_token_received &&
            (chunk.content_delta.has_value() || chunk.reasoning_content.has_value())) {
            first_token_time = std::chrono::steady_clock::now();
            first_token_received = true;
        }

        if (chunk.reasoning_content.has_value()) {
            std::cout << "\033[2m" << chunk.reasoning_content.value() << "\033[0m";
            std::cout.flush();
            token_count++;
        }

        if (chunk.content_delta.has_value()) {
            std::cout << chunk.content_delta.value();
            std::cout.flush();
            token_count++;
        }

        if (chunk.finish_reason.has_value()) {
            finish_reason = chunk.finish_reason.value();
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
        error_status = e.http_status;
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

    std::cout << "─── Streaming Response ─────────────────────────────────\n";

    int exit_code = 0;
    try {
        qai_forge::QaiForge::getInstance().generateStream(request, callbacks);

        std::unique_lock<std::mutex> lock(done_mutex);
        done_cv.wait(lock, [&] { return done; });

        if (had_error) {
            std::cerr << "\nGenAI error (HTTP " << error_status << "): " << error_message << "\n";
            exit_code = 1;
        }
    } catch (const GenAIException& e) {
        std::cerr << "\nGenAI error (HTTP " << e.http_status << "): " << e.message << "\n";
        exit_code = 1;
    } catch (const std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        exit_code = 1;
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

    qai_forge::QaiForge::getInstance().shutdown();
    return exit_code;
}
