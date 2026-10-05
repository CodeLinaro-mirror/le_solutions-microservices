// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// Example 05: Vision-Language Model (VLM) with Image Input
//
// Demonstrates multimodal inference using a VLM model (e.g. Qwen2.5-VL-3B).
// Images are passed as local file paths in the OpenAI image_url format.
// The SDK's model adapter transforms these into the model-specific vision
// tokens before sending to the VLM worker.
//
// Key concepts:
//   - OpenAI image_url content format in messages
//   - QaiForge::generateStream() + StreamCallbacks (same facade as 01-04)
//   - Model adapters transform image_url → vision tokens automatically
//
// Model selection (checked in order):
//   1. argv[1]              e.g. ./05_vlm qwen2.5-vl-3b
//   2. QAI_FORGE_MODEL env var
//   3. Fallback default: "qwen2.5-vl-3b"
//
// Usage:
//   ./05_vlm [model_id] [image_path] [question]
//   ./05_vlm qwen2.5-vl-3b /tmp/photo.jpg "What is in this image?"
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/QaiForge.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/InternalDTOs.h"
#include <iostream>
#include <string>
#include <fstream>
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

// Check if a file exists and is readable
bool file_exists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

} // namespace

int main(int argc, char* argv[]) {
    const std::string model_id   = resolveModelId(argc, argv, "qwen2.5-vl-3b");
    const std::string image_path = (argc > 2) ? argv[2] : "/tmp/test_image.jpg";
    const std::string question   = (argc > 3) ? argv[3] : "Describe what you see in this image.";

    std::cout << "=== qai-forge Example 05: Vision-Language Model ===\n\n";
    std::cout << "Model:      " << model_id
               << " (override with argv[1] or QAI_FORGE_MODEL env var)\n";
    std::cout << "Image:      " << image_path << "\n";
    std::cout << "Question:   " << question << "\n\n";

    qai_forge::QaiForge::getInstance().start();

    // Initialize model registry
    ModelConfigManager::getInstance().scanModelBundles();
    if (!ModelConfigManager::getInstance().validateModel(model_id)) {
        std::cerr << "ERROR: Model '" << model_id << "' not found.\n";
        qai_forge::QaiForge::getInstance().shutdown();
        return 1;
    }

    // Verify this is a VLM model
    if (!ModelConfigManager::getInstance().supportsVision(model_id)) {
        std::cerr << "ERROR: Model '" << model_id << "' does not support vision.\n";
        std::cerr << "VLM models available:\n";
        for (const auto& mc : ModelConfigManager::getInstance().getAvailableModels()) {
            if (mc.supports_vision) {
                std::cerr << "  - " << mc.id << " (" << mc.display_name << ")\n";
            }
        }
        qai_forge::QaiForge::getInstance().shutdown();
        return 1;
    }

    // Verify the image file exists
    if (!file_exists(image_path)) {
        std::cerr << "ERROR: Image file not found: " << image_path << "\n";
        std::cerr << "Please provide a valid image path as the second argument.\n";
        qai_forge::QaiForge::getInstance().shutdown();
        return 1;
    }

    // ── Build the request with image_url content ───────────────────────────────
    // The OpenAI image_url format uses a content array with mixed text and
    // image parts. The SDK's model adapter transforms these into the
    // model-specific vision token format.
    //
    // For local files, use the "file://" scheme or just the absolute path.
    // The VLM worker loads the file directly from disk.
    CreateChatCompletionRequest request;
    request.model = model_id;
    request.stream = true;
    request.max_completion_tokens = 512;
    request.temperature = 0.7f;

    // Build message with mixed text + image content
    request.messages = {
        {{"role", "system"}, {"content", "You are a helpful vision assistant."}},
        {{"role", "user"}, {"content", json::array({
            // Text part
            {{"type", "text"}, {"text", question}},
            // Image part — local file path
            {{"type", "image_url"}, {"image_url", {{"url", image_path}}}}
        })}}
    };

    // ── Run streaming inference ────────────────────────────────────────────────
    std::cout << "─── VLM Response ───────────────────────────────────────\n";

    int token_count = 0;
    std::mutex done_mutex;
    std::condition_variable done_cv;
    bool done = false;
    bool had_error = false;
    std::string error_message;
    int error_status = 0;

    qai_forge::StreamCallbacks callbacks;
    callbacks.onToken = [&](const StreamChunk& chunk) {
        if (chunk.content_delta.has_value()) {
            std::cout << chunk.content_delta.value();
            std::cout.flush();
            token_count++;
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

    std::cout << "\n────────────────────────────────────────────────────────\n";
    std::cout << "Tokens generated: " << token_count << "\n";

    qai_forge::QaiForge::getInstance().shutdown();
    return exit_code;
}
