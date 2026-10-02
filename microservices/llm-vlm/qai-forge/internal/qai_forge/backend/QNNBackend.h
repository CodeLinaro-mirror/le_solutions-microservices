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
// QNNBackend — qai_forge::IInferenceBackend implementation for QNN Predictive AI
//
// Uses a shared PredictiveWorkerChannel (via PredictiveWorkerProcessPool) to
// manage the ONE qnn-inference-worker process hosting ALL QNN-backed
// resident models — not one process per model. Supports QNN context binary
// (.bin) models compiled for HTP/GPU/CPU backends.
//
// Placement: initialize(RuntimeLaunchRequest) translates the resolved
// ExecutionPlan into a QNN device_id INIT option via QnnLaunchOptions.
//
// metadata.json fields used:
//   "runtime": "qnn" or "qnn_context_binary"
//   "config_file": absolute path to .bin context binary
//
// Environment variable overrides (used only to seed automatic plans):
//   QNN_WORKER_BINARY, QNN_BACKEND_LIB, QNN_SYS_LIB
// ─────────────────────────────────────────────────────────────────────────────

class QNNBackend : public qai_forge::IInferenceBackend,
                    public IPlacementAwarePredictiveBackend {
public:
    /**
     * Public constructor — creates an owned (non-singleton) instance.
     * Used by BackendFactory::createPredictiveBackend() for scheduler-owned
     * instances. Each PredictiveModelRuntime owns its own QNNBackend.
     */
    QNNBackend();

    static QNNBackend& getInstance();

    std::string name() const override { return "QNN"; }

    void initialize(const std::string& model_id,
                    const std::string& model_file) override;

    void initialize(
        const qai_forge::execution::RuntimeLaunchRequest& request) override;

    /**
     * Placement-aware initialization invoked by PredictiveModelRuntime when
     * its runtime has received a placement plan.
     */
    void initialize(const std::string& model_id,
                    const std::string& model_file,
                    const qai_forge::execution::ExecutionPlan& plan,
                    qai_forge::execution::RuntimeId runtime_id = {});

    TensorInferenceResponse infer(
        const SegmentedTensorInferenceRequest& request) override;
    PredictiveExecuteRequest prepareWorkerRequest(
        const SegmentedTensorInferenceRequest& request) override;

    bool isHealthy() const override;
    void shutdown() override;

private:
    QNNBackend(const QNNBackend&) = delete;
    QNNBackend& operator=(const QNNBackend&) = delete;

    std::unique_ptr<PredictiveSharedMemory> shared_memory_;
    // Shared across ALL QNN-backed models in this process — obtained lazily
    // from PredictiveWorkerProcessPool, never owned exclusively by this
    // backend instance. Do not close/terminate it from here.
    PredictiveWorkerChannel* channel_ = nullptr;
    qai_forge::execution::ExecutionPlan current_plan_;
    qai_forge::execution::RuntimeId current_runtime_id_;
    bool has_current_plan_ = false;
    std::string current_model_id_;
};
