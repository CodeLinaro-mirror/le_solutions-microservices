// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/scheduler/CancelResult.h"
#include "qai_forge/scheduler/ConversationMemoryCoordinator.h"
#include "qai_forge/scheduler/PriorityModelQueue.h"
#include "qai_forge/orchestration/IGenerativeOrchestrator.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class IGenerativeBackend;

namespace scheduler {

class PostTurnWorker;

enum class ModelRuntimeState {
    NotResident,
    Loading,
    Idle,
    Running,
    PostTurn,
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
};

struct ModelRuntimeEvents {
    std::function<void(const std::string& model_id, ModelRuntimeState state)>
        on_state_changed;
    std::function<void()> acquire_load_permit;
    std::function<void()> release_load_permit;
};

// Owns one model's queue and resident backend lifecycle.
//
// A runtime can exist while cold: jobs are queued, but
// backend.loadModel(model_id) only happens after admission calls activate().
//
// Each ModelRuntime shares one IGenerativeOrchestrator instance (created by
// BackendFactory::createRuntimePair). The orchestrator is stateless and
// receives the backend as an injected parameter on each execute() call.
class ModelRuntime {
public:
    ModelRuntime(std::string model_id,
                 std::unique_ptr<IGenerativeBackend> backend,
                 std::shared_ptr<IGenerativeOrchestrator> orchestrator,
                 std::shared_ptr<ConversationMemoryCoordinator> coordinator,
                 ModelRuntimeEvents events = {});
    ~ModelRuntime();

    ModelRuntime(const ModelRuntime&) = delete;
    ModelRuntime& operator=(const ModelRuntime&) = delete;
    ModelRuntime(ModelRuntime&&) = delete;
    ModelRuntime& operator=(ModelRuntime&&) = delete;

    void start();
    void enqueue(GenerativeJobPtr job);
    bool activate();
    CancelResult cancel(const std::string& job_id);
    void requestDrain();
    void failQueued(const GenAIException& error);
    void stop(bool force = false);

    ModelRuntimeSnapshot snapshot() const;
    ModelRuntimeAdmissionSnapshot admissionSnapshot() const;
    std::string modelId() const;
    ModelRuntimeState state() const;

private:
    enum class DrainMode {
        None,
        Normal,
    };

    void executorLoop();
    size_t promoteAgedJobs();
    bool runJob(GenerativeJob& job);
    bool beginPostTurn(GenerativeJob& job,
                       const StandardResponse& response);
    void finishPostTurn();
    void unloadBackend(bool force);
    void armCancelWatchdog(const std::string& job_id);
    void invalidateCancelWatchdog();
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
    PriorityModelQueue queue_;
    std::unique_ptr<IGenerativeBackend> backend_;
    std::shared_ptr<IGenerativeOrchestrator> orchestrator_;
    std::shared_ptr<ConversationMemoryCoordinator> memory_coordinator_;
    std::unique_ptr<PostTurnWorker> post_turn_worker_;
    ModelRuntimeEvents events_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread executor_thread_;

    bool started_ = false;
    bool stop_requested_ = false;
    bool force_stop_ = false;
    bool activation_requested_ = false;
    DrainMode drain_mode_ = DrainMode::None;
    std::chrono::milliseconds new_request_aging_threshold_ =
        std::chrono::seconds(30);
    std::chrono::milliseconds cancel_grace_period_ =
        std::chrono::seconds(30);

    ModelRuntimeState state_ = ModelRuntimeState::NotResident;
    bool backend_healthy_ = false;
    GenerativeJobPtr running_job_;

    std::mutex cancel_watchdog_mutex_;
    std::condition_variable cancel_watchdog_cv_;
    std::thread cancel_watchdog_thread_;
    std::uint64_t cancel_watchdog_generation_ = 0;
    bool cancel_watchdog_shutdown_ = false;
};

} // namespace scheduler
