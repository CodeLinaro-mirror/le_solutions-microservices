// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/orchestration/PredictiveOrchestrator.h"
#include "qai_forge/scheduler/ModelRuntime.h"
#include "qai_forge/scheduler/PredictiveJob.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class IInferenceBackend;

namespace scheduler {

// Owns one predictive model's bounded FIFO and backend lifecycle. One executor
// thread performs cold load and inference serially for that model.
class PredictiveModelRuntime {
public:
    PredictiveModelRuntime(
        std::string model_id,
        std::unique_ptr<IInferenceBackend> backend,
        std::shared_ptr<PredictiveOrchestrator> orchestrator,
        ModelRuntimeEvents events = {});
    ~PredictiveModelRuntime();

    PredictiveModelRuntime(const PredictiveModelRuntime&) = delete;
    PredictiveModelRuntime& operator=(const PredictiveModelRuntime&) = delete;
    PredictiveModelRuntime(PredictiveModelRuntime&&) = delete;
    PredictiveModelRuntime& operator=(PredictiveModelRuntime&&) = delete;

    void start();
    bool enqueue(PredictiveJobPtr job, size_t max_queue_depth);

    // IInferenceBackend cannot interrupt a running infer(), so force=true still
    // joins the executor and waits for in-flight work before backend teardown.
    void stop(bool force = false);

    ModelRuntimeSnapshot snapshot() const;
    ModelRuntimeState state() const;
    std::string modelId() const;
    std::chrono::steady_clock::time_point lastUsedAt() const;

private:
    void executorLoop();
    void ensureModelLoaded();
    bool unloadBackend();
    void recoverBackend();
    void setState(ModelRuntimeState state);
    void notifyStateChanged(ModelRuntimeState state);
    void failJobs(std::deque<PredictiveJobPtr> jobs,
                  const GenAIException& error);

    static void notifyComplete(const PredictiveJobPtr& job,
                               const TensorInferenceResponse& response);
    static void notifyError(const PredictiveJobPtr& job,
                            const GenAIException& error);

    const std::string model_id_;
    std::unique_ptr<IInferenceBackend> backend_;
    std::shared_ptr<PredictiveOrchestrator> orchestrator_;
    ModelRuntimeEvents events_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread executor_thread_;
    std::deque<PredictiveJobPtr> queue_;
    PredictiveJobPtr running_job_;

    bool started_ = false;
    bool stop_requested_ = false;
    bool backend_started_ = false;
    bool use_lock_written_ = false;
    ModelRuntimeState state_ = ModelRuntimeState::NotResident;
    bool backend_healthy_ = false;
    std::chrono::steady_clock::time_point last_used_at_ =
        std::chrono::steady_clock::now();
};

} // namespace scheduler
