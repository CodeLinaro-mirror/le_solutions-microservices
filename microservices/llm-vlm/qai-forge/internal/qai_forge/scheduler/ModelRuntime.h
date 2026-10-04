// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/backend/BackendAdapter.h"
#include "qai_forge/execution/ExecutionPlan.h"
#include "qai_forge/scheduler/CancelResult.h"
#include "qai_forge/scheduler/ModelLoadCoordinator.h"
#include "qai_forge/scheduler/PlacementAdmissionGateway.h"
#include "qai_forge/scheduler/PriorityModelQueue.h"
#include "qai_forge/orchestration/OrchestratorAdapter.h"
#include "qai_forge/utils/ThreadPool.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <unordered_set>

namespace scheduler {

class PostTurnWorker;

enum class ModelRuntimeState {
    NotResident,
    Loading,
    Idle,
    Running,
    Draining,
    Evicting,
    Failed,
    Stopped
};

struct ModelRuntimeSnapshot {
    std::string model_id;
    ModelRuntimeState state = ModelRuntimeState::NotResident;
    QueueSnapshot queue;
    bool has_running_job = false;
    std::string running_job_id;
    bool healthy = false;
};

struct ModelRuntimeAdmissionSnapshot {
    std::string model_id;
    ModelRuntimeState state = ModelRuntimeState::NotResident;
    QueueSnapshot queue;
    QueueAdmissionCandidate candidate;
    bool has_running_job = false;
    std::string running_job_id;
    bool healthy = false;
    // Earliest GenerativeJob::enqueue_deadline across all currently-queued
    // jobs for this model, or time_point::max() if none are queued or the
    // admission-timeout feature is disabled (see
    // WarmModelPoolConfig::queue_admission_timeout). Lets WarmModelPool's
    // event loop wake up exactly when the next expiration is due.
    std::chrono::steady_clock::time_point earliest_queue_deadline =
        std::chrono::steady_clock::time_point::max();
};

struct ModelRuntimeEvents {
    std::function<void(const std::string& model_id, ModelRuntimeState state)>
        on_state_changed;
};

// Owns one model's queue and resident backend lifecycle.
//
// A runtime can exist while cold: jobs are queued, but
// backend.loadModel(model_id) only happens after admission calls activate().
//
// Each ModelRuntime shares one OrchestratorAdapter instance (created by
// BackendFactory::createRuntimePair, wrapping whatever
// qai_forge::IGenerativeOrchestrator was registered for this model's
// runtime). The orchestrator is stateless and receives the backend as an
// injected parameter on each execute() call.
class ModelRuntime {
public:
    ModelRuntime(std::string model_id,
                 std::unique_ptr<BackendAdapter> backend,
                 std::shared_ptr<OrchestratorAdapter> orchestrator,
                 std::shared_ptr<ModelLoadCoordinator> load_coordinator,
                 long model_memory_mb,
                 std::shared_ptr<PlacementAdmissionGateway> placement,
                 ModelRuntimeEvents events = {});
    ~ModelRuntime();

    ModelRuntime(const ModelRuntime&) = delete;
    ModelRuntime& operator=(const ModelRuntime&) = delete;
    ModelRuntime(ModelRuntime&&) = delete;
    ModelRuntime& operator=(ModelRuntime&&) = delete;

    void start();
    void enqueue(GenerativeJobPtr job);
    bool activate();
    bool activate(ModelLoadCoordinator::LoadReservation reservation);
    CancelResult cancel(const std::string& job_id);
    void requestDrain();
    void failQueued(const GenAIException& error);

    // Selectively fails (via on_error) every queued job whose
    // GenerativeJob::enqueue_deadline has passed, instead of draining the
    // entire queue like failQueued() does. No-op if the admission-timeout
    // feature is disabled (jobs default to enqueue_deadline ==
    // time_point::max(), which never compares <= now). Called from
    // WarmModelPool's event loop on every policy tick when
    // WarmModelPoolConfig::queue_admission_timeout > 0.
    void expireStaleQueuedJobs();

    // Earliest enqueue_deadline across this runtime's queued jobs, or
    // time_point::max() if none are queued or the feature is disabled.
    std::chrono::steady_clock::time_point earliestQueueDeadline() const;

    void stop(bool force = false);

    ModelRuntimeSnapshot snapshot() const;
    ModelRuntimeAdmissionSnapshot admissionSnapshot() const;
    std::string modelId() const;
    ModelRuntimeState state() const;

    /**
     * Forward clearSession(session_id) to the backend.
     * Thread-safe — acquires no locks, backend is responsible for its own sync.
     */
    void clearSession(const std::string& session_id);

private:
    enum class DrainMode {
        None,
        Normal,
    };

    void executorLoop();
    void runJobEntry(GenerativeJobPtr job);
    bool runJob(GenerativeJob& job);
    bool beginPostTurn(GenerativeJob& job,
                       const StandardResponse& response);
    void finishPostTurn(const std::string& session_id);
    void unloadBackend(bool force);
    void armCancelWatchdog(const std::string& job_id);
    void invalidateCancelWatchdog(const std::string& job_id);
    void stopCancelWatchdog();
    void cancelWatchdogLoop(std::string job_id, std::uint64_t generation);
    void handleCancelWatchdogTimeout(const std::string& job_id);
    void setStateLocked(ModelRuntimeState state,
                        std::vector<ModelRuntimeState>& state_events);
    void notifyStateChanged(ModelRuntimeState state);
    void notifyStateChanges(const std::vector<ModelRuntimeState>& state_events);

    static bool isResidentState(ModelRuntimeState state);
    static void notifyCancelled(const GenerativeJobPtr& job);
    static void notifyError(const GenerativeJobPtr& job,
                            const GenAIException& error);

    const std::string model_id_;
    const qai_forge::execution::RuntimeId runtime_id_;
    PriorityModelQueue queue_;
    std::unique_ptr<BackendAdapter> backend_;
    std::shared_ptr<OrchestratorAdapter> orchestrator_;
    std::shared_ptr<ModelLoadCoordinator> load_coordinator_;
    std::shared_ptr<PlacementAdmissionGateway> placement_;
    PlacementReservation placement_reservation_;
    std::unique_ptr<PostTurnWorker> post_turn_worker_;
    ModelRuntimeEvents events_;
    long model_memory_mb_ = 4096;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread executor_thread_;

    bool started_ = false;
    bool stop_requested_ = false;
    bool force_stop_ = false;
    bool activation_requested_ = false;
    ModelLoadCoordinator::LoadReservation pending_load_reservation_;
    DrainMode drain_mode_ = DrainMode::None;
    std::chrono::milliseconds cancel_grace_period_ =
        std::chrono::seconds(30);

    ModelRuntimeState state_ = ModelRuntimeState::NotResident;
    bool backend_healthy_ = false;
    std::unordered_map<std::string, GenerativeJobPtr> running_jobs_;
    std::unordered_set<std::string> active_sessions_;
    std::unordered_set<std::string> post_turn_sessions_;
    std::size_t max_concurrent_ = 1;
    bool recovery_in_progress_ = false;

    // Bounded worker-thread pool used by executorLoop() to run admitted
    // jobs (see runJobEntry()) instead of spawning a new std::thread per
    // job. Jobs are already bounded in concurrency by max_concurrent_
    // admission gating above, so a pool sized to that same bound (grown
    // via resize() whenever max_concurrent_ increases after a backend
    // load — see the BATCH_CAPACITY log site in executorLoop()) provides
    // identical effective concurrency without per-job OS thread creation.
    // The cancel-watchdog force-kill path (armCancelWatchdog) is
    // deliberately NOT routed through this pool — it fires far less
    // frequently than job dispatch and has its own generation-based
    // replace/join lifecycle that is easier to reason about as a
    // dedicated std::thread per in-flight cancel.
    qai_forge::utils::ThreadPool job_pool_{1};

    struct CancelWatchdogEntry {
        std::thread thread;
        std::uint64_t generation = 0;
    };
    std::mutex cancel_watchdog_mutex_;
    std::condition_variable cancel_watchdog_cv_;
    std::unordered_map<std::string, CancelWatchdogEntry> cancel_watchdogs_;
    bool cancel_watchdog_shutdown_ = false;
};

} // namespace scheduler
