// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/dto/TensorDTOs.h"
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// PredictiveAIOrchestrator — Layer 2 orchestrator for Predictive AI
//
// Routes tensor inference requests to the correct IInferenceBackend based on
// the model's "runtime" field in metadata.json:
//   "qnn"  → QNNBackend  (QNN context binary, HTP/GPU/CPU)
//   "snpe" → SNPEBackend (SNPE DLC container, DSP/GPU/AIP/CPU)
//
// Unlike GenerativeOrchestrator, this orchestrator is stateless:
//   - No session management
//   - No context compaction
//   - No concurrency middleware (multiple requests can run concurrently)
//
// The transport layer (Layer 1) calls handleInfer() and receives a
// TensorInferenceResponse with raw output tensor bytes.
//
// See docs/unified-inference-service.md for the full design rationale.
// ─────────────────────────────────────────────────────────────────────────────

class PredictiveAIOrchestrator {
public:
    static PredictiveAIOrchestrator& getInstance();

    /**
     * Run Predictive AI inference.
     *
     * @param request  Input tensors (raw bytes) + model ID + output names
     * @return         Output tensors (raw bytes) + inference stats
     * @throws GenAIException on model-not-found or inference failure
     */
    TensorInferenceResponse handleInfer(const TensorInferenceRequest& request);

private:
    PredictiveAIOrchestrator() = default;
    PredictiveAIOrchestrator(const PredictiveAIOrchestrator&) = delete;
    PredictiveAIOrchestrator& operator=(const PredictiveAIOrchestrator&) = delete;
};
