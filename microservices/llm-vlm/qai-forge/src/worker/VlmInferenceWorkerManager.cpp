// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// VlmInferenceWorkerManager — Layer 3 VLM Subprocess Manager
//
// Extends InferenceWorkerManager for VLM inference. The key addition is
// executeVlmRequest() which appends image_urls to the EXECUTE command before
// sending it to the VLM worker subprocess.
//
// The VLM worker subprocess (genai-vlm-inference-worker) links against
// libvlmengine.so (VlmEngine) and handles the image encoding pipeline
// internally via GeniePipeline.
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/worker/VlmInferenceWorkerManager.h"
#include "qai_forge/worker/InferenceProtocol.h"
#include "qai_forge/utils/Logger.h"
#include <mutex>

// ─────────────────────────────────────────────────────────────────────────────
// Singleton
// ─────────────────────────────────────────────────────────────────────────────
VlmInferenceWorkerManager& VlmInferenceWorkerManager::getInstance() {
    static VlmInferenceWorkerManager instance;
    return instance;
}

// ─────────────────────────────────────────────────────────────────────────────
// executeVlmRequest — builds augmented EXECUTE command with image_urls and
//                     delegates to the protected sendExecuteAndStream helper.
//
// The base class executeRequest() builds its own command internally, so we
// cannot use it here — we must call sendExecuteAndStream() directly after
// constructing the augmented command.
// ─────────────────────────────────────────────────────────────────────────────
void VlmInferenceWorkerManager::executeVlmRequest(
    const std::string& event_id,
    const std::string& prompt,
    const std::vector<std::string>& image_urls,
    bool streaming,
    int max_tokens,
    float temperature,
    float top_p,
    int top_k,
    float presence_penalty,
    float frequency_penalty,
    TokenCallback on_token,
    DoneCallback on_done,
    ErrorCallback on_error) {

    std::lock_guard<std::mutex> lock(mutex_);
    is_active_ = true;

    // Build the EXECUTE command using the standard factory method
    json execute_cmd = InferenceProtocol::createExecuteCommand(
        event_id, prompt, streaming, max_tokens, temperature,
        top_p, top_k, presence_penalty, frequency_penalty,
        false  // bypass_think_filter — VLM models don't use thinking
    );

    // Append image URLs so the VLM worker can load and encode them
    if (!image_urls.empty()) {
        json urls_array = json::array();
        for (const auto& url : image_urls) {
            urls_array.push_back(url);
        }
        execute_cmd["image_urls"] = urls_array;
    }

    LOG_INFO("[VlmInferenceWorkerManager] Executing VLM request "
             << event_id << " with " << image_urls.size() << " image(s)");

    // Delegate to the protected helper — mutex_ is already held above
    sendExecuteAndStream(execute_cmd, event_id, on_token, on_done, on_error);

    is_active_ = false;
}
