// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// Example 05: Vision-Language Model (VLM) with Image Input
//
// Demonstrates multimodal inference using a VLM model (e.g. Qwen2.5-VL-3B).
// Images are passed as local file paths in the OpenAI image_url format.
// The SDK's model adapter (Qwen25Adapter) transforms these into the
// model-specific vision tokens before sending to the VLM worker.
//
// Key concepts:
//   - OpenAI image_url content format in messages
//   - VlmInferenceWorkerManager: spawns genai-vlm-inference-worker
//   - image_refs in EXECUTE command (shared-memory offsets) → content_items in Query struct
//   - Qwen25Adapter::preprocessVision() transforms image_url → vision tokens
//
// Usage:
//   ./05_vlm [model_id] [image_path] [question]
//   ./05_vlm qwen2.5-vl-3b /tmp/photo.jpg "What is in this image?"
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/orchestration/ChatOrchestrator.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/InternalDTOs.h"
#include <iostream>
#include <string>
#include <fstream>

// Check if a file exists and is readable
static bool file_exists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

int main(int argc, char* argv[]) {
    const std::string model_id   = (argc > 1) ? argv[1] : "qwen2.5-vl-3b";
    const std::string image_path = (argc > 2) ? argv[2] : "/tmp/test_image.jpg";
    const std::string question   = (argc > 3) ? argv[3] : "Describe what you see in this image.";

    std::cout << "=== qai-forge Example 05: Vision-Language Model ===\n\n";
    std::cout << "Model:      " << model_id << "\n";
    std::cout << "Image:      " << image_path << "\n";
    std::cout << "Question:   " << question << "\n\n";

    // Initialize model registry
    ModelConfigManager::getInstance().scanModelBundles();
    if (!ModelConfigManager::getInstance().validateModel(model_id)) {
        std::cerr << "ERROR: Model '" << model_id << "' not found.\n";
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
        return 1;
    }

    // Verify the image file exists
    if (!file_exists(image_path)) {
        std::cerr << "ERROR: Image file not found: " << image_path << "\n";
        std::cerr << "Please provide a valid image path as the second argument.\n";
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
    try {
        ChatOrchestrator::getInstance().handleStreaming(request,
            [&](const StreamChunk& chunk) {
                if (chunk.content_delta.has_value()) {
                    std::cout << chunk.content_delta.value();
                    std::cout.flush();
                    token_count++;
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

    std::cout << "\n────────────────────────────────────────────────────────\n";
    std::cout << "Tokens generated: " << token_count << "\n";

    return 0;
}
