// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/dto/TensorDTOs.h"
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// IInferenceBackend — Layer 3 interface for Predictive AI inference
//
// Implemented by:
//   LiteRTBackend — wraps TFLite C++ API (TfLiteInterpreter)
//   QNNBackend    — wraps QNN SDK C++ API
//
// Design invariants:
//   - PredictiveAIOrchestrator ONLY calls methods on this interface.
//   - Tensor data is always raw bytes — no base64, no JSON arrays.
//   - The backend runs in a subprocess (same fault isolation as GenIEBackend).
//   - infer() is blocking — returns when all outputs are ready.
//
// Contrast with IGenerativeBackend:
//   - IGenerativeBackend: text in → streaming tokens out (stateful, sessions)
//   - IInferenceBackend:  tensors in → tensors out (stateless, no sessions)
//
// See docs/unified-inference-service.md §5 for the full design rationale.
// ─────────────────────────────────────────────────────────────────────────────

class IInferenceBackend {
public:
    virtual ~IInferenceBackend() = default;

    // Human-readable name for logging ("LiteRT", "QNN")
    virtual std::string name() const = 0;

    /**
     * Initialize the backend with a model.
     * Called once when the model is first loaded.
     * Throws std::runtime_error on failure.
     *
     * @param model_id    Model identifier
     * @param model_file  Absolute path to the model file (.tflite, .bin, etc.)
     */
    virtual void initialize(const std::string& model_id,
                            const std::string& model_file) = 0;

    /**
     * Run blocking inference.
     * Returns when all requested outputs are ready.
     * Throws std::runtime_error on inference failure.
     *
     * @param request  Input tensors (raw bytes) + output names to return
     * @return         Output tensors (raw bytes) + inference stats
     */
    virtual TensorInferenceResponse infer(const TensorInferenceRequest& request) = 0;

    /**
     * Returns true if the backend worker subprocess is alive and responsive.
     */
    virtual bool isHealthy() const = 0;

    /**
     * Graceful shutdown — sends SHUTDOWN command to worker, waits for exit.
     */
    virtual void shutdown() = 0;
};
