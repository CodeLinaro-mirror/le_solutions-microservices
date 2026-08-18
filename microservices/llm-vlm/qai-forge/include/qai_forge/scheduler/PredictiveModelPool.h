// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/scheduler/PredictiveModelRuntime.h"
#include "qai_forge/scheduler/WarmModelPool.h"
#include "qai_forge/backend/BackendFactory.h"
#include "qai_forge/dto/TensorDTOs.h"
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace scheduler {

class EvictionPolicy;

// ─────────────────────────────────────────────────────────────────────────────
// PredictiveModelPool — Manages warm predictive model residency
//
// Similar to WarmModelPool but for predictive AI models (classification,
// detection, segmentation). Key differences:
//   - No job queuing — infer() is synchronous
//   - No tool-chain leases — predictive models are stateless
//   - Simpler lifecycle — no activation/drain coordination
//
// The pool manages:
//   - Multiple PredictiveModelRuntime instances (one per loaded model)
//   - LRU eviction when max_active_models is exceeded
//   - Idle-timeout eviction for unused models
//   - Memory headroom enforcement
//
// Concurrency model:
//   - Different models run concurrently (each in its own PredictiveModelRuntime)
//   - Same model requests are serialized (by PredictiveModelRuntime's mutex)
//
// Example:
//   Request A: mobilenet-v3 (QNN)  → PredictiveModelRuntime A → runs concurrently
//   Request B: resnet-50 (QNN)     → PredictiveModelRuntime B → runs concurrently
//   Request C: mobilenet-v3 (QNN)  → PredictiveModelRuntime A → serialized
//
// Thread safety: All public methods are thread-safe.
// ─────────────────────────────────────────────────────────────────────────────
class PredictiveModelPool {
public:
    /**
     * Construct a pool with the given configuration.
     *
     * @param config   Pool configuration (max models, idle timeout, etc.)
     * @param factory  Optional factory for creating backend instances.
     *                 If not provided, uses BackendFactory::createPredictiveBackendForModel.
     */
    explicit PredictiveModelPool(WarmModelPoolConfig config,
                                 PredictiveBackendFactory factory = {});

    ~PredictiveModelPool();

    PredictiveModelPool(const PredictiveModelPool&) = delete;
    PredictiveModelPool& operator=(const PredictiveModelPool&) = delete;
    PredictiveModelPool(PredictiveModelPool&&) = delete;
    PredictiveModelPool& operator=(PredictiveModelPool&&) = delete;

    /**
     * Run synchronous tensor inference.
     *
     * If the model is not yet loaded, this will:
     *   1. Evict idle models if needed to make room
     *   2. Create a new PredictiveModelRuntime
     *   3. Load the model (backend.initialize)
     *   4. Run inference
     *
     * If the model is already loaded, this will:
     *   1. Route to the existing PredictiveModelRuntime
     *   2. Run inference (serialized with other requests for the same model)
     *
     * Thread safety: Multiple concurrent calls are safe. Different models
     * run concurrently; same model requests are serialized.
     *
     * @param request  Input tensors + model ID + output names
     * @return         Output tensors + inference stats
     * @throws GenAIException on model-not-found, eviction failure, or inference error
     */
    TensorInferenceResponse infer(const TensorInferenceRequest& request);

    /**
     * Get a snapshot of the pool state (for monitoring/debugging).
     */
    ModelPoolSnapshot snapshot() const;

    /**
     * Stop the pool and unload all models.
     *
     * @param force  If true, abort in-flight inference immediately.
     *               If false, wait for in-flight inference to complete.
     */
    void stop(bool force = false);

private:
    struct RuntimeRecord {
        std::unique_ptr<PredictiveModelRuntime> runtime;
        ModelRuntimeState state = ModelRuntimeState::NotResident;
        std::chrono::steady_clock::time_point last_used_at =
            std::chrono::steady_clock::now();
        std::optional<std::chrono::steady_clock::time_point> idle_since;
    };

    PredictiveModelRuntime& getOrLoadModel(const std::string& model_id);
    void evictIfNeeded(const std::string& model_id_to_load);
    void evictIdleModels();
    RuntimeRecord& createRuntimeLocked(const std::string& model_id);
    void handleRuntimeStateChanged(const std::string& model_id,
                                    ModelRuntimeState state);
    size_t activeModelCount() const;
    long availableMemoryMb() const;
    long modelMemoryMb(const std::string& model_id) const;

    WarmModelPoolConfig config_;
    PredictiveBackendFactory factory_;
    std::unique_ptr<EvictionPolicy> eviction_policy_;

    mutable std::mutex mutex_;
    bool shutdown_requested_ = false;
    std::unordered_map<std::string, RuntimeRecord> runtimes_;
};

} // namespace scheduler
