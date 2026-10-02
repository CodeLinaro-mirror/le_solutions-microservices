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
// LiteRTBackend — qai_forge::IInferenceBackend implementation for LiteRT Predictive AI
//
// Uses a shared PredictiveWorkerChannel (via PredictiveWorkerProcessPool) to
// manage the ONE litert-inference-worker process hosting ALL LiteRT-backed
// resident models — not one process per model. The worker binary path is
// resolved from the LITERT_WORKER_BINARY environment variable (default:
// /usr/local/bin/litert-inference-worker).
//
// Placement: initialize(RuntimeLaunchRequest) translates the resolved
// ExecutionPlan into a LiteRT hw_accelerators bitmask via LiteRtLaunchOptions.
//
// LOAD_MODEL params sent to the worker:
//   model_file          — absolute path to the .tflite model
//   dispatch_lib_dir    — directory containing LiteRT dispatch .so
//   compiler_plugin_dir — directory containing LiteRT compiler plugin .so
//   hw_accelerators     — bitmask (NPU=4, CPU=1; default 5 = NPU|CPU)
// ─────────────────────────────────────────────────────────────────────────────

class LiteRTBackend : public qai_forge::IInferenceBackend,
                       public IPlacementAwarePredictiveBackend {
public:
    /**
     * Public constructor — creates an owned (non-singleton) instance.
     * Used by BackendFactory::createPredictiveBackend() for scheduler-owned
     * instances. Each PredictiveModelRuntime owns its own LiteRTBackend.
     */
    LiteRTBackend();

    static LiteRTBackend& getInstance();

    std::string name() const override { return "LiteRT"; }

    void initialize(const std::string& model_id,
                    const std::string& model_file) override;

    void initialize(
        const qai_forge::execution::RuntimeLaunchRequest& request) override;

    /**
     * Placement-aware initialization. LiteRT accepts accelerator classes,
     * not physical HTP IDs; binding remains Automatic.
     */
    void initialize(const std::string& model_id,
                    const std::string& model_file,
                    const qai_forge::execution::ExecutionPlan& plan);

    TensorInferenceResponse infer(
        const SegmentedTensorInferenceRequest& request) override;
    PredictiveExecuteRequest prepareWorkerRequest(
        const SegmentedTensorInferenceRequest& request) override;

    bool isHealthy() const override;
    void shutdown() override;

private:
    LiteRTBackend(const LiteRTBackend&) = delete;
    LiteRTBackend& operator=(const LiteRTBackend&) = delete;

    std::unique_ptr<PredictiveSharedMemory> shared_memory_;
    // Shared across ALL LiteRT-backed models in this process — obtained
    // lazily from PredictiveWorkerProcessPool, never owned exclusively by
    // this backend instance. Do not close/terminate it from here.
    PredictiveWorkerChannel* channel_ = nullptr;
    qai_forge::execution::ExecutionPlan current_plan_;
    bool has_current_plan_ = false;
    std::string current_model_id_;
};
