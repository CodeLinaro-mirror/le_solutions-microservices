// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/scheduler/ModelRuntime.h"
#include "qai_forge/dto/TensorDTOs.h"
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

class IInferenceBackend;

namespace scheduler {

// Reuse ModelRuntimeState from ModelRuntime.h
// Reuse ModelRuntimeSnapshot structure (subset of fields used)
// Reuse ModelRuntimeEvents for state change notifications

// ─────────────────────────────────────────────────────────────────────────────
// PredictiveModelRuntime — Owns one predictive model's backend lifecycle
//
// Similar to ModelRuntime but for tensor inference (classification, detection,
// segmentation). Key differences:
//   - No job queue — infer() is synchronous and blocks until complete
//   - No executor thread — each infer() call runs in the caller's thread
//   - Concurrent requests for the same model are serialized via mutex
//   - Different models can run concurrently (handled at pool level)
//
// Lifecycle:
//   1. Construct with model_id + owned IInferenceBackend
//   2. start() — transitions to NotResident (ready to load)
//   3. First infer() call triggers backend.initialize() → Loading → Idle
//   4. Subsequent infer() calls reuse the loaded model
//   5. stop() — unloads the model and shuts down the backend
//
// Thread safety: All public methods are thread-safe. Multiple threads may call
// infer() concurrently; they will be serialized via the internal mutex.
// ─────────────────────────────────────────────────────────────────────────────
class PredictiveModelRuntime {
public:
    /**
     * Construct a runtime for the given model.
     *
     * @param model_id  Model identifier (from ModelConfig)
     * @param backend   Owned IInferenceBackend instance (QNN, SNPE, LiteRT)
     * @param events    Optional callbacks for state changes
     */
    PredictiveModelRuntime(std::string model_id,
                           std::unique_ptr<IInferenceBackend> backend,
                           ModelRuntimeEvents events = {});

    ~PredictiveModelRuntime();

    PredictiveModelRuntime(const PredictiveModelRuntime&) = delete;
    PredictiveModelRuntime& operator=(const PredictiveModelRuntime&) = delete;
    PredictiveModelRuntime(PredictiveModelRuntime&&) = delete;
    PredictiveModelRuntime& operator=(PredictiveModelRuntime&&) = delete;

    /**
     * Start the runtime (transitions to NotResident state).
     * Must be called before infer().
     */
    void start();

    /**
     * Run synchronous tensor inference.
     *
     * If the model is not yet loaded (NotResident), this will:
     *   1. Transition to Loading
     *   2. Call backend.initialize(model_id, model_file)
     *   3. Transition to Idle
     *   4. Run inference
     *
     * If the model is already loaded (Idle), this will:
     *   1. Transition to Running
     *   2. Call backend.infer(request)
     *   3. Transition back to Idle
     *   4. Return the response
     *
     * Thread safety: Multiple concurrent calls are serialized via mutex.
     *
     * @param request  Input tensors + model ID + output names
     * @return         Output tensors + inference stats
     * @throws GenAIException on model load failure or inference error
     */
    TensorInferenceResponse infer(const TensorInferenceRequest& request);

    /**
     * Stop the runtime and unload the model.
     *
     * @param force  If true, abort immediately even if inference is running.
     *               If false, wait for any in-flight inference to complete.
     */
    void stop(bool force = false);

    /**
     * Get a snapshot of the current runtime state.
     * Used by PredictiveModelPool for eviction decisions.
     */
    ModelRuntimeSnapshot snapshot() const;

    /**
     * Get the current state.
     */
    ModelRuntimeState state() const;

    /**
     * Get the model ID.
     */
    std::string modelId() const;

private:
    void loadModelLocked();
    void unloadBackend(bool force);
    void setStateLocked(ModelRuntimeState new_state);
    void notifyStateChanged(ModelRuntimeState state);

    const std::string model_id_;
    std::unique_ptr<IInferenceBackend> backend_;
    ModelRuntimeEvents events_;

    mutable std::mutex mutex_;
    bool started_ = false;
    bool stop_requested_ = false;
    ModelRuntimeState state_ = ModelRuntimeState::NotResident;
    bool backend_healthy_ = false;
    std::chrono::steady_clock::time_point last_used_at_;
};

} // namespace scheduler
