// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/backend/IInferenceBackend.h"
#include "qai_forge/execution/ExecutionPlan.h"

#include <memory>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// PredictiveBackendAdapter — internal glue between the public qai_forge::
// qai_forge::IInferenceBackend ABI and the scheduler.
//
// Mirrors BackendAdapter's role for the generative side — every concrete
// predictive backend (QNN/SNPE/LiteRT, and any future third-party plugin)
// implements the public interface; PluginRegistry wraps the registered
// instance in exactly one of these before handing it to
// PredictiveModelRuntime. Adds the placement-aware initialize() entry point
// the scheduler actually needs, via opportunistic dynamic_cast to
// IPlacementAwarePredictiveBackend (falls back to the public
// initialize(model_id, {}) for backends that don't implement that mixin).
// ─────────────────────────────────────────────────────────────────────────────
class PredictiveBackendAdapter {
public:
    explicit PredictiveBackendAdapter(
        std::unique_ptr<qai_forge::IInferenceBackend> impl);

    // Placement-aware initialize. Falls back to
    // impl_->initialize(request.model_id, {}) if impl_ doesn't also
    // implement IPlacementAwarePredictiveBackend.
    void initialize(const qai_forge::execution::RuntimeLaunchRequest& request);

    // Forwarded 1:1 to the wrapped public backend — same signatures as
    // qai_forge::IInferenceBackend.
    std::string name() const;
    void initialize(const std::string& model_id, const std::string& model_file);
    TensorInferenceResponse infer(const SegmentedTensorInferenceRequest& request);
    PredictiveExecuteRequest prepareWorkerRequest(
        const SegmentedTensorInferenceRequest& request);
    bool isHealthy() const;
    void shutdown();

    // Scheduler/orchestrator code needs the raw public reference to hand to
    // PredictiveOrchestrator::executeBatch() (which takes
    // qai_forge::IInferenceBackend& per the public ABI, never
    // PredictiveBackendAdapter).
    qai_forge::IInferenceBackend& publicBackend() { return *impl_; }

private:
    std::unique_ptr<qai_forge::IInferenceBackend> impl_;
};
