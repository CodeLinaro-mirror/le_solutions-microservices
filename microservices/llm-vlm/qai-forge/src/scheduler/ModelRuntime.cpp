// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/scheduler/ModelRuntime.h"

#include "qai_forge/backend/IGenerativeBackend.h"
#include "qai_forge/orchestration/IOrchestrator.h"
#include "qai_forge/utils/Logger.h"

#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace scheduler {

namespace {

GenAIException stoppedError(const std::string& model_id) {
    return GenAIException(
        GenAIErrorCode::INTERNAL_ERROR,
        "ModelRuntime for model '" + model_id + "' stopped before execution",
        500);
}

GenAIException loadFailureError(const std::string& model_id,
                                const std::exception& error) {
    return GenAIException(
        GenAIErrorCode::INTERNAL_ERROR,
        "Failed to load model runtime '" + model_id + "': " + error.what(),
        500);
}

const char* stateToString(ModelRuntimeState state) {
    switch (state) {
        case ModelRuntimeState::NotResident:
            return "not_resident";
        case ModelRuntimeState::Loading:
            return "loading";
        case ModelRuntimeState::Idle:
            return "idle";
        case ModelRuntimeState::Running:
            return "running";
        case ModelRuntimeState::Draining:
            return "draining";
        case ModelRuntimeState::Evicting:
            return "evicting";
        case ModelRuntimeState::Failed:
            return "failed";
        case ModelRuntimeState::Stopped:
            return "stopped";
    }
    return "unknown";
}

const char* runningCancelModeToString(RunningCancelMode mode) {
    switch (mode) {
        case RunningCancelMode::SOFT:
            return "soft";
        case RunningCancelMode::HARD:
            return "hard";
    }
    return "soft";
}

long parseLongEnv(const char* name, long fallback) {
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }

    try {
        return std::stol(value);
    } catch (...) {
        return fallback;
    }
}

std::chrono::milliseconds queueAgingThresholdFromEnv() {
    const long seconds = parseLongEnv("MODEL_QUEUE_AGING_SECONDS", 30);
    if (seconds <= 0) {
        return std::chrono::milliseconds(0);
    }
    return std::chrono::seconds(seconds);
}

bool shouldRecoverBackend(const GenAIException& error) {
    return error.http_status >= 500;
}

// After any worker subprocess teardown (load failure or eviction), the DSP
// kernel needs time to reclaim that process's FastRPC/SMMU mappings before a
// new worker can safely map the same memory. Without this delay, a load
// attempted right after teardown can fail with e.g. "fastrpc memory map
// failed" / "[LlmEngine] Failed to create dialog", since the previous
// worker's mappings haven't been reclaimed yet.
// Default: 2000ms. Override: MODEL_LOAD_FAILURE_DELAY_MS env var.
void waitForDspMemoryReclaim(const std::string& model_id, bool stop_requested) {
    const long delay_ms = parseLongEnv("MODEL_LOAD_FAILURE_DELAY_MS", 2000);
    if (delay_ms > 0 && !stop_requested) {
        LOG_INFO("[ModelRuntime] Waiting " << delay_ms
                 << "ms after backend teardown for DSP memory reclaim: model="
                 << model_id);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
}

} // namespace

ModelRuntime::ModelRuntime(std::string model_id,
                     std::unique_ptr<IGenerativeBackend> backend,
                     std::unique_ptr<IOrchestrator> orchestrator,
                     ModelRuntimeEvents events,
                     RunningCancelMode running_cancel_mode)
    : model_id_(std::move(model_id)),
      backend_(std::move(backend)),
      orchestrator_(std::move(orchestrator)),
      events_(std::move(events)),
      new_request_aging_threshold_(queueAgingThresholdFromEnv()),
      running_cancel_mode_(running_cancel_mode) {
    if (model_id_.empty()) {
        throw std::invalid_argument("ModelRuntime requires a non-empty model id");
    }
    if (!backend_) {
        throw std::invalid_argument("ModelRuntime requires a model backend");
    }
    if (!orchestrator_) {
        throw std::invalid_argument("ModelRuntime requires an orchestrator");
    }
}

ModelRuntime::~ModelRuntime() {
    stop(false);
}

void ModelRuntime::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
        return;
    }

    stop_requested_ = false;
    force_stop_ = false;
    started_ = true;
    executor_thread_ = std::thread(&ModelRuntime::executorLoop, this);
    LOG_INFO("[ModelRuntime] Started executor: model=" << model_id_);
}

void ModelRuntime::enqueue(InferenceJobPtr job) {
    const std::string job_id = job ? job->job_id : std::string("<null>");
    queue_.push(std::move(job));
    LOG_INFO("[ModelRuntime] Enqueued job: model=" << model_id_
             << " job=" << job_id);
    cv_.notify_one();
}

bool ModelRuntime::activate() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == ModelRuntimeState::Stopped || stop_requested_) {
            LOG_WARN("[ModelRuntime] Activate ignored because runtime is stopping: model="
                     << model_id_ << " state=" << stateToString(state_));
            return false;
        }

        if (state_ == ModelRuntimeState::Loading ||
            state_ == ModelRuntimeState::Idle ||
            state_ == ModelRuntimeState::Running ||
            state_ == ModelRuntimeState::Draining) {
            drain_mode_ = DrainMode::None;
            return true;
        }

        if (state_ == ModelRuntimeState::Evicting) {
            activation_requested_ = true;
            drain_mode_ = DrainMode::None;
            return true;
        }

        activation_requested_ = true;
        drain_mode_ = DrainMode::None;
    }

    cv_.notify_one();
    return true;
}

CancelResult ModelRuntime::cancel(const std::string& job_id) {
    if (job_id.empty()) {
        return CancelResult{
            CancelStatus::NOT_FOUND,
            job_id,
            "Cancel request is missing job_id"};
    }

    if (InferenceJobPtr queued = queue_.cancel(job_id)) {
        LOG_INFO("[ModelRuntime] Cancelled queued job: model=" << model_id_
                 << " job=" << job_id);
        notifyCancelled(queued);
        return CancelResult{
            CancelStatus::QUEUED_CANCELLED,
            job_id,
            "Queued scheduler job cancelled"};
    }

    InferenceJobPtr running;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_job_ && running_job_->job_id == job_id) {
            running = running_job_;
            running->cancel();
        }
    }

    if (!running) {
        return CancelResult{
            CancelStatus::NOT_FOUND,
            job_id,
            "Scheduler job is not queued or running for this model"};
    }

    LOG_WARN("[ModelRuntime] Cancelling running job: model=" << model_id_
             << " job=" << job_id
             << " mode=" << runningCancelModeToString(running_cancel_mode_));
    notifyCancelled(running);

    if (running_cancel_mode_ == RunningCancelMode::HARD) {
        try {
            backend_->terminateWorker(/*force=*/true);
        } catch (...) {
        }
    }

    return CancelResult{
        CancelStatus::RUNNING_CANCELLED,
        job_id,
        running_cancel_mode_ == RunningCancelMode::HARD
            ? "Running scheduler job cancelled and worker abort requested"
            : "Running scheduler job soft-cancelled"};
}

void ModelRuntime::requestDrain() {
    std::vector<ModelRuntimeState> state_events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == ModelRuntimeState::Stopped || stop_requested_) {
            return;
        }

        activation_requested_ = false;
        if (state_ == ModelRuntimeState::NotResident ||
            state_ == ModelRuntimeState::Failed) {
            return;
        }

        drain_mode_ = DrainMode::Normal;
        LOG_INFO("[ModelRuntime] Drain requested: model=" << model_id_
                 << " state=" << stateToString(state_));
        if (state_ == ModelRuntimeState::Running) {
            setStateLocked(ModelRuntimeState::Draining, state_events);
        }
    }

    notifyStateChanges(state_events);
    cv_.notify_one();
}

void ModelRuntime::requestProtectedDrain() {
    std::vector<ModelRuntimeState> state_events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == ModelRuntimeState::Stopped || stop_requested_) {
            return;
        }

        activation_requested_ = false;
        if (state_ == ModelRuntimeState::NotResident ||
            state_ == ModelRuntimeState::Failed ||
            state_ == ModelRuntimeState::Evicting) {
            return;
        }

        drain_mode_ = DrainMode::Protected;
        LOG_INFO("[ModelRuntime] Protected drain requested: model=" << model_id_
                 << " state=" << stateToString(state_));
        if (state_ == ModelRuntimeState::Running) {
            setStateLocked(ModelRuntimeState::Draining, state_events);
        }
    }

    notifyStateChanges(state_events);
    cv_.notify_one();
}

void ModelRuntime::failQueued(const GenAIException& error) {
    while (InferenceJobPtr job = queue_.pop()) {
        notifyError(job, error);
    }
}

void ModelRuntime::stop(bool force) {
    std::string running_job_id;
    bool should_join = false;
    bool was_started = false;
    std::vector<ModelRuntimeState> state_events;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            setStateLocked(ModelRuntimeState::Stopped, state_events);
        } else {
            was_started = true;
            stop_requested_ = true;
            force_stop_ = force_stop_ || force;
            activation_requested_ = false;
            drain_mode_ = DrainMode::None;

            if (force && running_job_) {
                running_job_id = running_job_->job_id;
                running_job_->cancel();
            }

            should_join = executor_thread_.joinable() &&
                          executor_thread_.get_id() != std::this_thread::get_id();
        }
    }

    notifyStateChanges(state_events);

    if (!was_started) {
        return;
    }

    if (force && !running_job_id.empty()) {
        try {
            backend_->terminateWorker(/*force=*/true);
        } catch (...) {
        }
    }

    cv_.notify_one();

    if (should_join) {
        executor_thread_.join();
    }
}

ModelRuntimeSnapshot ModelRuntime::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    ModelRuntimeSnapshot snapshot;
    snapshot.model_id = model_id_;
    snapshot.state = state_;
    snapshot.queue = queue_.snapshot();
    snapshot.has_running_job = static_cast<bool>(running_job_);
    if (running_job_) {
        snapshot.running_job_id = running_job_->job_id;
    }
    snapshot.healthy = backend_healthy_;
    return snapshot;
}

ModelRuntimeAdmissionSnapshot ModelRuntime::admissionSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    ModelRuntimeAdmissionSnapshot snapshot;
    snapshot.model_id = model_id_;
    snapshot.state = state_;
    snapshot.queue = queue_.snapshot();
    snapshot.candidate = queue_.admissionCandidate();
    snapshot.has_running_job = static_cast<bool>(running_job_);
    if (running_job_) {
        snapshot.running_job_id = running_job_->job_id;
    }
    snapshot.healthy = backend_healthy_;
    return snapshot;
}

std::string ModelRuntime::modelId() const {
    return model_id_;
}

ModelRuntimeState ModelRuntime::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void ModelRuntime::executorLoop() {
    while (true) {
        bool should_load = false;
        bool should_unload = false;
        InferenceJobPtr job;
        std::vector<ModelRuntimeState> state_events;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() {
                return stop_requested_ ||
                       activation_requested_ ||
                       drain_mode_ != DrainMode::None ||
                       (state_ == ModelRuntimeState::Idle && !queue_.empty());
            });

            if (stop_requested_) {
                break;
            }

            if (drain_mode_ != DrainMode::None &&
                !isResidentState(state_)) {
                drain_mode_ = DrainMode::None;
            }

            if (activation_requested_ &&
                (state_ == ModelRuntimeState::NotResident ||
                 state_ == ModelRuntimeState::Failed)) {
                activation_requested_ = false;
                backend_healthy_ = false;
                setStateLocked(ModelRuntimeState::Loading, state_events);
                should_load = true;
            } else if (drain_mode_ == DrainMode::Normal &&
                       isResidentState(state_)) {
                drain_mode_ = DrainMode::None;
                setStateLocked(ModelRuntimeState::Evicting, state_events);
                should_unload = true;
            } else if (drain_mode_ == DrainMode::Protected &&
                       isResidentState(state_) &&
                       (state_ == ModelRuntimeState::Idle ||
                        state_ == ModelRuntimeState::Draining)) {
                promoteAgedJobs();
                job = queue_.popProtectedDrainJob();
                if (job) {
                    running_job_ = job;
                    setStateLocked(ModelRuntimeState::Running, state_events);
                } else {
                    drain_mode_ = DrainMode::None;
                    setStateLocked(ModelRuntimeState::Evicting, state_events);
                    should_unload = true;
                }
            } else if (state_ == ModelRuntimeState::Idle) {
                promoteAgedJobs();
                job = queue_.pop();
                if (job) {
                    running_job_ = job;
                    setStateLocked(ModelRuntimeState::Running, state_events);
                }
            }
        }

        notifyStateChanges(state_events);

        if (should_load) {
            try {
                LOG_INFO("[ModelRuntime] Loading backend: model=" << model_id_);
                backend_->loadModel(model_id_);
                LOG_INFO("[ModelRuntime] Backend loaded: model=" << model_id_);
                std::vector<ModelRuntimeState> load_events;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!stop_requested_) {
                        backend_healthy_ = true;
                        setStateLocked(ModelRuntimeState::Idle, load_events);
                    }
                }
                notifyStateChanges(load_events);
                cv_.notify_one();
            } catch (const GenAIException& error) {
                LOG_ERROR("[ModelRuntime] Backend load failed: model=" << model_id_
                          << " status=" << error.http_status
                          << " message=\"" << error.message << "\"");
                // Force-kill the worker subprocess (SIGKILL) so the kernel
                // reclaims all its file descriptors and DSP SMMU mappings.
                unloadBackend(/*force=*/true);
                waitForDspMemoryReclaim(model_id_, stop_requested_);
                std::vector<ModelRuntimeState> failure_events;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    backend_healthy_ = false;
                    setStateLocked(ModelRuntimeState::Failed, failure_events);
                }
                notifyStateChanges(failure_events);
                failQueued(error);
            } catch (const std::exception& error) {
                LOG_ERROR("[ModelRuntime] Backend load failed: model=" << model_id_
                          << " message=\"" << error.what() << "\"");
                // Force-kill the worker subprocess (SIGKILL).
                unloadBackend(/*force=*/true);
                waitForDspMemoryReclaim(model_id_, stop_requested_);
                std::vector<ModelRuntimeState> failure_events;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    backend_healthy_ = false;
                    setStateLocked(ModelRuntimeState::Failed, failure_events);
                }
                notifyStateChanges(failure_events);
                failQueued(loadFailureError(model_id_, error));
            }
            continue;
        }

        if (should_unload) {
            LOG_INFO("[ModelRuntime] Unloading backend: model=" << model_id_);
            unloadBackend(false);
            waitForDspMemoryReclaim(model_id_, stop_requested_);
            std::vector<ModelRuntimeState> unload_events;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                backend_healthy_ = false;
                if (!stop_requested_) {
                    setStateLocked(ModelRuntimeState::NotResident, unload_events);
                }
            }
            notifyStateChanges(unload_events);
            continue;
        }

        if (!job) {
            continue;
        }

        if (job->isCancelled()) {
            notifyCancelled(job);
            std::vector<ModelRuntimeState> cancel_events;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                running_job_.reset();
                if (!stop_requested_) {
                    setStateLocked(ModelRuntimeState::Idle, cancel_events);
                }
            }
            notifyStateChanges(cancel_events);
            continue;
        }

        bool recover_backend = false;
        try {
            LOG_INFO("[ModelRuntime] Running job: model=" << model_id_
                     << " job=" << job->job_id
                     << " priority=" << static_cast<int>(job->priority)
                     << " stream=" << (job->request.stream ? "true" : "false"));
            runJob(*job);
            LOG_INFO("[ModelRuntime] Job run returned: model=" << model_id_
                     << " job=" << job->job_id);
        } catch (const GenAIException& error) {
            recover_backend = shouldRecoverBackend(error);
            LOG_WARN("[ModelRuntime] Job failed: model=" << model_id_
                     << " job=" << job->job_id
                     << " status=" << error.http_status
                     << " recover_backend="
                     << (recover_backend ? "true" : "false")
                     << " cancelled="
                     << (job->isCancelled() ? "true" : "false")
                     << " message=\"" << error.message << "\"");
            if (!job->isCancelled()) {
                notifyError(job, error);
            }
        } catch (const std::exception& error) {
            recover_backend = true;
            LOG_WARN("[ModelRuntime] Job failed with runtime exception: model="
                     << model_id_
                     << " job=" << job->job_id
                     << " recover_backend=true"
                     << " cancelled="
                     << (job->isCancelled() ? "true" : "false")
                     << " message=\"" << error.what() << "\"");
            if (!job->isCancelled()) {
                notifyError(
                    job,
                    GenAIException(
                        GenAIErrorCode::INTERNAL_ERROR,
                        "Model runtime execution failed for job '" + job->job_id +
                            "': " + error.what(),
                        500));
            }
        }

        if (recover_backend) {
            LOG_WARN("[ModelRuntime] Recovering backend after execution failure: model="
                     << model_id_ << " job=" << job->job_id);
            unloadBackend(true);
            waitForDspMemoryReclaim(model_id_, stop_requested_);
        }

        std::vector<ModelRuntimeState> complete_events;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_job_.reset();
            if (recover_backend) {
                backend_healthy_ = false;
                if (!stop_requested_) {
                    setStateLocked(ModelRuntimeState::Failed, complete_events);
                }
            } else if (!stop_requested_) {
                setStateLocked(
                    drain_mode_ != DrainMode::None ? ModelRuntimeState::Draining
                                                   : ModelRuntimeState::Idle,
                    complete_events);
            }
        }

        notifyStateChanges(complete_events);
        cv_.notify_one();
    }

    bool force_unload = false;
    std::vector<ModelRuntimeState> exit_events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        force_unload = force_stop_;
        if (isResidentState(state_)) {
            setStateLocked(ModelRuntimeState::Evicting, exit_events);
        }
    }

    notifyStateChanges(exit_events);
    unloadBackend(force_unload);
    failQueued(stoppedError(model_id_));

    std::vector<ModelRuntimeState> stopped_events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_job_.reset();
        backend_healthy_ = false;
        setStateLocked(ModelRuntimeState::Stopped, stopped_events);
        started_ = false;
    }
    notifyStateChanges(stopped_events);
    LOG_INFO("[ModelRuntime] Executor stopped: model=" << model_id_
             << " force_unload=" << (force_unload ? "true" : "false"));
}

size_t ModelRuntime::promoteAgedJobs() {
    if (new_request_aging_threshold_.count() <= 0) {
        return 0;
    }

    const size_t promoted = queue_.promoteAgedNewRequests(
        std::chrono::steady_clock::now(),
        new_request_aging_threshold_);
    if (promoted > 0) {
        LOG_INFO("[ModelRuntime] Promoted aged queued jobs: model=" << model_id_
                 << " count=" << promoted
                 << " threshold_ms=" << new_request_aging_threshold_.count());
    }
    return promoted;
}

void ModelRuntime::runJob(InferenceJob& job) {
    if (!backend_healthy_ || !backend_ || !backend_->isHealthy()) {
        throw GenAIException(
            GenAIErrorCode::HARDWARE_UNAVAILABLE,
            "ModelRuntime backend is not loaded for scheduled execution.",
            503);
    }

    if (!job.model_id.empty() && job.model_id != model_id_) {
        throw GenAIException(
            GenAIErrorCode::INTERNAL_ERROR,
            "ModelRuntime loaded model '" + model_id_ +
                "' cannot execute job for model '" + job.model_id + "'.",
            500);
    }

    if (job.request.model.empty()) {
        job.request.model = model_id_;
    }

    auto notify_cancelled = [&job]() {
        if (job.markCancellationNotified() && job.callbacks.on_cancelled) {
            job.callbacks.on_cancelled();
        }
    };

    auto cancel_requested = [&job]() -> bool {
        return job.isCancelled();
    };

    if (job.isCancelled()) {
        notify_cancelled();
        return;
    }

    // Build the response_history: use job's history if use_response_history is
    // set, otherwise pass an empty array (orchestrator creates a fresh session).
    const json& response_history =
        job.use_response_history ? job.response_history : json::array();

    // Build the stream callback: non-null for streaming jobs, null for blocking.
    OrchestratorStreamCallback stream_callback = nullptr;
    std::string finish_reason = "stop";
    std::string response_id = job.session_id;

    if (job.request.stream) {
        stream_callback = [&job, &finish_reason, &response_id](
                              const StreamChunk& chunk) {
            if (chunk.finish_reason.has_value()) {
                finish_reason = chunk.finish_reason.value();
            }
            if (!chunk.id.empty()) {
                response_id = chunk.id;
            }
            if (job.isCancelled()) {
                return;
            }
            if (job.callbacks.on_token) {
                job.callbacks.on_token(chunk);
            }
        };
    }

    try {
        if (job.request.stream) {
            LOG_INFO("[ModelRuntime] Streaming execution started: job="
                     << job.job_id << " model=" << model_id_);
        } else {
            LOG_INFO("[ModelRuntime] Blocking execution started: job="
                     << job.job_id << " model=" << model_id_);
        }

        StandardResponse response = orchestrator_->execute(
            job.request,
            response_history,
            *backend_,
            stream_callback,
            cancel_requested);

        if (job.isCancelled()) {
            notify_cancelled();
            return;
        }

        if (job.callbacks.on_complete) {
            if (response.id.empty()) {
                response.id = response_id;
            }
            if (response.finish_reason.empty()) {
                response.finish_reason = finish_reason;
            }
            job.callbacks.on_complete(response);
        }

        if (job.request.stream) {
            LOG_INFO("[ModelRuntime] Streaming execution completed: job="
                     << job.job_id << " model=" << model_id_
                     << " finish_reason=" << finish_reason);
        } else {
            LOG_INFO("[ModelRuntime] Blocking execution completed: job="
                     << job.job_id << " model=" << model_id_
                     << " finish_reason=" << response.finish_reason);
        }
    } catch (...) {
        if (job.isCancelled()) {
            notify_cancelled();
            return;
        }
        LOG_WARN("[ModelRuntime] Execution threw: job=" << job.job_id
                 << " model=" << model_id_);
        throw;
    }
}

void ModelRuntime::unloadBackend(bool force) {
    try {
        backend_->unloadModel(force);
        LOG_INFO("[ModelRuntime] Backend unloaded: model=" << model_id_
                 << " force=" << (force ? "true" : "false"));
    } catch (...) {
        LOG_WARN("[ModelRuntime] Backend unload threw and was suppressed: model="
                 << model_id_);
    }
}

void ModelRuntime::setStateLocked(
    ModelRuntimeState state,
    std::vector<ModelRuntimeState>& state_events) {
    if (state_ == state) {
        return;
    }

    const ModelRuntimeState old_state = state_;
    state_ = state;
    state_events.push_back(state);
    LOG_INFO("[ModelRuntime] State transition: model=" << model_id_
             << " " << stateToString(old_state)
             << " -> " << stateToString(state_));
}

void ModelRuntime::notifyStateChanged(ModelRuntimeState state) {
    if (!events_.on_state_changed) {
        return;
    }

    try {
        events_.on_state_changed(model_id_, state);
    } catch (...) {
    }
}

void ModelRuntime::notifyStateChanges(
    const std::vector<ModelRuntimeState>& state_events) {
    for (ModelRuntimeState state : state_events) {
        notifyStateChanged(state);
    }
}

bool ModelRuntime::isResidentState(ModelRuntimeState state) {
    return state == ModelRuntimeState::Loading ||
           state == ModelRuntimeState::Idle ||
           state == ModelRuntimeState::Running ||
           state == ModelRuntimeState::Draining ||
           state == ModelRuntimeState::Evicting;
}

void ModelRuntime::notifyCancelled(const InferenceJobPtr& job) {
    if (!job || !job->markCancellationNotified() ||
        !job->callbacks.on_cancelled) {
        return;
    }

    try {
        job->callbacks.on_cancelled();
    } catch (...) {
    }
}

void ModelRuntime::notifyError(const InferenceJobPtr& job,
                            const GenAIException& error) {
    if (!job || !job->callbacks.on_error) {
        return;
    }

    try {
        job->callbacks.on_error(error);
    } catch (...) {
    }
}

} // namespace scheduler
