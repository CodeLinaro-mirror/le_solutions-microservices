// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/backend/IInferenceBackend.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include <memory>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// LiteRTBackend — IInferenceBackend implementation for LiteRT Predictive AI
//
// Wraps PredictiveWorkerManager to manage the litert-inference-worker
// subprocess. Supports .tflite models with NPU acceleration (Qualcomm QNN
// dispatch) and CPU fallback via the LiteRT CompiledModel API.
//
// metadata.json fields used:
//   "runtime":              "litert"
//   "model_file":           "model.tflite"
//   "dispatch_lib_dir":     "/usr/lib"   (optional, default /usr/lib)
//   "compiler_plugin_dir":  "/usr/lib"   (optional, default /usr/lib)
//
// Environment variable overrides:
//   LITERT_WORKER_BINARY       — path to litert-inference-worker binary
//   LITERT_DISPATCH_DIR        — dispatch library directory
//   LITERT_COMPILER_PLUGIN_DIR — compiler plugin directory
// ─────────────────────────────────────────────────────────────────────────────

class PredictiveWorkerManager;

class LiteRTBackend : public IInferenceBackend {
public:
    static LiteRTBackend& getInstance();

    std::string name() const override { return "LiteRT"; }

    void initialize(const std::string& model_id,
                    const std::string& model_file) override;

    TensorInferenceResponse infer(const TensorInferenceRequest& request) override;

    bool isHealthy() const override;
    void shutdown() override;

private:
    LiteRTBackend();
    LiteRTBackend(const LiteRTBackend&)            = delete;
    LiteRTBackend& operator=(const LiteRTBackend&) = delete;

    std::unique_ptr<PredictiveWorkerManager> worker_;
    std::string current_model_id_;
};
