// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/worker/InferenceWorkerManager.h"
#include <string>
#include <vector>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// VlmInferenceWorkerManager — Layer 3 VLM Subprocess Manager (P5)
//
// Extends InferenceWorkerManager for VLM (Vision-Language Model) inference.
// The key difference from the LLM manager is that VLM requests may include
// image data that must be passed to the worker subprocess.
//
// Preprocessed image tensors are copied into the base class's image
// shared-memory region (see InferenceWorkerManager header Section H) and
// referenced in the EXECUTE command as an "image_refs" array of
// {"offset","len"} pairs — no file paths or inline base64 are sent over IPC.
//
// The VLM worker subprocess links against libvlmservice.so (vlm-interface.h)
// instead of libllmservice.so (llm-interface.h).
//
// Architecture compliance (Section 4.E):
//   - Uses a separate UUID socket path: /tmp/genai-vlm-worker-{model_id}-{uuid}.sock
//   - Inherits all fault isolation and cancellation semantics from the base class.
// ─────────────────────────────────────────────────────────────────────────────
class VlmInferenceWorkerManager : public InferenceWorkerManager {
public:
    VlmInferenceWorkerManager() : InferenceWorkerManager("vlm") {}

    /**
     * Execute a VLM inference request with optional image data.
     *
     * @param event_id          Unique ID for this inference event
     * @param prompt            The compacted context prompt (text portion)
     * @param images             Preprocessed image tensors (already encoded
     *                          by the model adapter's preprocessVision()),
     *                          one entry per image in request order
     * @param streaming         Whether to stream tokens
     * @param max_tokens        Max completion tokens
     * @param temperature       Sampling temperature
     * @param top_p             Top-p sampling
     * @param top_k             Top-k sampling
     * @param presence_penalty  Presence penalty
     * @param frequency_penalty Frequency penalty
     * @param on_token          Called for each TOKEN event
     * @param on_done           Called when DONE event received
     * @param on_error          Called on ERROR event or socket failure
     */
    void executeVlmRequest(const std::string& event_id,
                           const std::string& prompt,
                           const std::vector<std::vector<uint8_t>>& images,
                           bool streaming,
                           int max_tokens,
                           float temperature,
                           float top_p,
                           int top_k,
                           float presence_penalty,
                           float frequency_penalty,
                           TokenCallback on_token,
                           DoneCallback on_done,
                           ErrorCallback on_error);

    /**
     * Get the singleton VLM worker manager instance.
     */
    static VlmInferenceWorkerManager& getInstance();
};
