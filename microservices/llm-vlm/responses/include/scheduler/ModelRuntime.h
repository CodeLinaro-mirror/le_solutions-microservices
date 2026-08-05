// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "scheduler/CancelResult.h"
#include "scheduler/PriorityModelQueue.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class IGenerativeBackend;

namespace scheduler {

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
};

struct ModelRuntimeEvents {
    std::function<void(const std::string& model_id, ModelRuntimeState state)>
        on_state_changed;
};

// Owns one model's queue and resident backend lifecycle.
//
// A runtime can exist while cold: jobs are queued, but
// backend.loadModel(model_id) only happens after admission calls activate().
class ModelRuntime {
public:
    ModelRuntime(std::string model_id,
                 std::unique_ptr<IGenerativeBackend> backend,
                 ModelRuntimeEvents events = {},
                 RunningCancelMode running_cancel_mode =
                     RunningCancelMode::SOFT);
    ~ModelRuntime();

    ModelRuntime(const ModelRuntime&) = delete;
    ModelRuntime& operator=(const ModelRuntime&) = delete;
    ModelRuntime(ModelRuntime&&) = delete;
    ModelRuntime& operator=(ModelRuntime&&) = delete;

    void start();
    void enqueue(InferenceJobPtr job);
    bool activate();
    CancelResult cancel(const std::string& job_id);
    void requestDrain();
    void requestProtectedDrain();
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
        Protected,
    };

    void executorLoop();
    size_t promoteAgedJobs();
    void runJob(InferenceJob& job);
    void unloadBackend(bool force);
    void setStateLocked(ModelRuntimeState state,
                        std::vector<ModelRuntimeState>& state_events);
    void notifyStateChanged(ModelRuntimeState state);
    void notifyStateChanges(const std::vector<ModelRuntimeState>& state_events);

    static bool isResidentState(ModelRuntimeState state);
    static void notifyCancelled(const InferenceJobPtr& job);
    static void notifyError(const InferenceJobPtr& job, const GenAIException& error);

    const std::string model_id_;
    PriorityModelQueue queue_;
    std::unique_ptr<IGenerativeBackend> backend_;
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
    RunningCancelMode running_cancel_mode_ = RunningCancelMode::SOFT;

    ModelRuntimeState state_ = ModelRuntimeState::NotResident;
    bool backend_healthy_ = false;
    InferenceJobPtr running_job_;
};

} // namespace scheduler
