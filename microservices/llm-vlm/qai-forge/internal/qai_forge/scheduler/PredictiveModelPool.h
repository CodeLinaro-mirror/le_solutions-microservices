// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/scheduler/PredictiveModelRuntime.h"
#include "qai_forge/scheduler/PredictiveJob.h"
#include "qai_forge/scheduler/PlacementAdmissionGateway.h"
#include "qai_forge/scheduler/WarmModelPool.h"
#include "qai_forge/scheduler/IRequestRoutingPolicy.h"
#include "qai_forge/backend/BackendFactory.h"
#include "qai_forge/dto/TensorDTOs.h"
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace scheduler {

// ─────────────────────────────────────────────────────────────────────────────
// PredictiveModelPool — Manages warm predictive model residency
//
// Similar to WarmModelPool but for predictive AI models (classification,
// detection, segmentation). Key differences:
//   - One bounded FIFO per model; public infer() remains synchronous
//   - No tool-chain leases — predictive models are stateless
//   - Simpler lifecycle — no activation/drain coordination
//
// The pool manages:
//   - Multiple PredictiveModelRuntime instances (one per loaded replica)
//   - LRU eviction when max_active_models is exceeded
//   - Idle-timeout eviction for unused models
//   - Memory headroom enforcement
//
// Replica model: mirrors WarmModelPool — one model_id may have
// 1..max_replicas_per_model concurrently-resident PredictiveModelRuntime
// instances, each identified by a distinct
// qai_forge::execution::RuntimeId{model_id, replica_id}. With the default
// max_replicas_per_model == 1 this class behaves identically to the
// single-runtime-per-model design.
//
// Concurrency model:
//   - Different models (or replicas) run concurrently (each in its own
//     PredictiveModelRuntime)
//   - Same-replica requests are serialized by that runtime's executor thread
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
                                 PredictiveBackendFactory factory = {},
                                 ModelRuntimeEvents runtime_events = {},
                                 std::shared_ptr<ModelLoadCoordinator>
                                     load_coordinator = {},
                                 std::shared_ptr<PlacementAdmissionGateway>
                                     placement = {},
                                 std::shared_ptr<IRequestRoutingPolicy>
                                     routing_policy = {});

    ~PredictiveModelPool();

    PredictiveModelPool(const PredictiveModelPool&) = delete;
    PredictiveModelPool& operator=(const PredictiveModelPool&) = delete;
    PredictiveModelPool(PredictiveModelPool&&) = delete;
    PredictiveModelPool& operator=(PredictiveModelPool&&) = delete;

    /** @brief Reserve a predictive runtime without passing request payload. */
    PredictiveModelRuntime* reserve(const std::string& model_id);

    /** @brief Submit a job to the reserved runtime's bounded FIFO. */
    SubmitResult submit(PredictiveModelRuntime* runtime,
                        PredictiveJobPtr job);

    /** @brief Release one pending runtime reservation. */
    void releaseReservation(PredictiveModelRuntime* runtime) noexcept;

    /**
     * Get a snapshot of the pool state (for monitoring/debugging).
     */
    ModelPoolSnapshot snapshot() const;

    /**
     * Stop the pool and unload all models.
     *
     * @param force  Preserved for API symmetry; predictive inference cannot be
     *               interrupted, so both modes wait for in-flight work.
     */
    void stop(bool force = false);

private:
    struct RuntimeRecord {
        std::unique_ptr<PredictiveModelRuntime> runtime;
        size_t reservation_count = 0;
    };

    void evictIfNeeded(const std::string& model_id_to_load);
    void evictIdleModels();
    RuntimeRecord& routeOrCreateRuntimeLocked(const std::string& model_id);
    RuntimeRecord& createRuntimeLocked(
        const qai_forge::execution::RuntimeId& runtime_id);
    RuntimeRecord* firstReplicaRecordLocked(const std::string& model_id);
    const std::vector<qai_forge::execution::RuntimeId>* replicasForModelLocked(
        const std::string& model_id) const;
    size_t activeModelCount() const;
    long availableMemoryMb() const;
    long modelMemoryMb(const std::string& model_id) const;

    WarmModelPoolConfig config_;
    PredictiveBackendFactory factory_;
    ModelRuntimeEvents runtime_events_;
    std::shared_ptr<ModelLoadCoordinator> load_coordinator_;
    bool owns_load_coordinator_ = false;
    std::shared_ptr<PlacementAdmissionGateway> placement_;
    std::shared_ptr<IRequestRoutingPolicy> routing_policy_;
    mutable std::mutex mutex_;
    bool shutdown_requested_ = false;
    std::unordered_map<qai_forge::execution::RuntimeId, RuntimeRecord,
                       qai_forge::execution::RuntimeIdHash> runtimes_;
    // Mirrors WarmModelPool::replicas_by_model_ — see its doc comment.
    std::unordered_map<std::string, std::vector<qai_forge::execution::RuntimeId>>
        replicas_by_model_;
};

} // namespace scheduler
