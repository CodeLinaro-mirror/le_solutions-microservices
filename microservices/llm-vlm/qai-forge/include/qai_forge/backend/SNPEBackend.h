// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/backend/IInferenceBackend.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include <memory>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// SNPEBackend — IInferenceBackend implementation for SNPE Predictive AI
//
// Wraps PredictiveWorkerManager to manage the snpe-inference-worker subprocess.
// Supports SNPE DLC container models with DSP/GPU/AIP/CPU delegates.
//
// metadata.json fields used:
//   "runtime":        "snpe"
//   "model_file":     "model.dlc"
//   "delegate":       "dsp"  (or "gpu", "aip", "none")
//   "output_tensors": ["output_0"]  (optional)
// ─────────────────────────────────────────────────────────────────────────────

class PredictiveWorkerManager;

class SNPEBackend : public IInferenceBackend {
public:
    /**
     * Public constructor — creates an owned (non-singleton) instance.
     * Used by BackendFactory::createPredictiveBackend() for scheduler-owned
     * instances. Each PredictiveModelRuntime owns its own SNPEBackend.
     */
    SNPEBackend();

    static SNPEBackend& getInstance();

    std::string name() const override { return "SNPE"; }

    void initialize(const std::string& model_id,
                    const std::string& model_file) override;

    TensorInferenceResponse infer(const TensorInferenceRequest& request) override;

    bool isHealthy() const override;
    void shutdown() override;

private:
    SNPEBackend(const SNPEBackend&) = delete;
    SNPEBackend& operator=(const SNPEBackend&) = delete;

    std::unique_ptr<PredictiveWorkerManager> worker_;
    std::string current_model_id_;
};
