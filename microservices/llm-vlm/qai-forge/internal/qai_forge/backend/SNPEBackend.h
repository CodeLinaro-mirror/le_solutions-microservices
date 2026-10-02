// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/backend/IInferenceBackend.h"
#include "qai_forge/backend/IPlacementAwarePredictiveBackend.h"
#include "qai_forge/execution/ExecutionPlan.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/worker/PredictiveWorkerChannel.h"
#include <memory>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// SNPEBackend — qai_forge::IInferenceBackend implementation for SNPE Predictive AI
//
// Uses a shared PredictiveWorkerChannel (via PredictiveWorkerProcessPool) to
// manage the ONE snpe-inference-worker process hosting ALL SNPE-backed
// resident models — not one process per model. Supports SNPE DLC container
// models with DSP/GPU/AIP/CPU delegates.
//
// Placement: initialize(RuntimeLaunchRequest) translates the resolved
// ExecutionPlan into an SNPE runtime_order LOAD_MODEL option via
// SnpeLaunchOptions.
//
// metadata.json fields used:
//   "runtime": "snpe" or "qnn_dlc"
//   "config_file": absolute path to .dlc container
//
// Environment variable overrides (used only to seed automatic plans):
//   SNPE_WORKER_BINARY, SNPE_DELEGATE
// ─────────────────────────────────────────────────────────────────────────────

class SNPEBackend : public qai_forge::IInferenceBackend,
                     public IPlacementAwarePredictiveBackend {
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

    void initialize(
        const qai_forge::execution::RuntimeLaunchRequest& request) override;

    // Placement-aware additive overload. The common backend interface remains
    // unchanged; the SNPE adapter materializes RuntimePreference plans into
    // native worker LOAD_MODEL options.
    void initialize(
        const std::string& model_id,
        const std::string& model_file,
        const qai_forge::execution::ExecutionPlan& plan);

    TensorInferenceResponse infer(
        const SegmentedTensorInferenceRequest& request) override;
    PredictiveExecuteRequest prepareWorkerRequest(
        const SegmentedTensorInferenceRequest& request) override;

    bool isHealthy() const override;
    void shutdown() override;

private:
    SNPEBackend(const SNPEBackend&) = delete;
    SNPEBackend& operator=(const SNPEBackend&) = delete;

    std::unique_ptr<PredictiveSharedMemory> shared_memory_;
    // Shared across ALL SNPE-backed models in this process — obtained lazily
    // from PredictiveWorkerProcessPool, never owned exclusively by this
    // backend instance. Do not close/terminate it from here.
    PredictiveWorkerChannel* channel_ = nullptr;
    qai_forge::execution::ExecutionPlan current_plan_;
    bool has_current_plan_ = false;
    std::string current_model_id_;
};
