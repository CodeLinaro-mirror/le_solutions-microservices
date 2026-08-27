// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/scheduler/ModelRuntime.h"

#include "qai_forge/backend/IGenerativeBackend.h"
#include "qai_forge/orchestration/IGenerativeOrchestrator.h"
#include "qai_forge/scheduler/PostTurnWorker.h"
#include "qai_forge/utils/Logger.h"
#include "qai_forge/utils/UseLock.h"

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
        case ModelRuntimeState::PostTurn:
            return "post_turn";
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

std::chrono::milliseconds cancelGracePeriodFromEnv() {
    const long ms = parseLongEnv("RESPONSES_CANCEL_GRACE_MS", 30000);
    if (ms < 0) {
        return std::chrono::milliseconds(30000);
    }
    return std::chrono::milliseconds(ms);
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
// Default: 2000ms. Override: DSP_RECLAIM_DELAY_MS env var.
void waitForDspMemoryReclaim(const std::string& model_id, bool stop_requested) {
    const long delay_ms = parseLongEnv("DSP_RECLAIM_DELAY_MS", 2000);
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
                     std::shared_ptr<IGenerativeOrchestrator> orchestrator,
                     std::shared_ptr<ConversationMemoryCoordinator> coordinator,
                     std::shared_ptr<ModelLoadCoordinator> load_coordinator,
                     long model_memory_mb,
                     ModelRuntimeEvents events)
    : model_id_(std::move(model_id)),
      backend_(std::move(backend)),
      orchestrator_(std::move(orchestrator)),
      memory_coordinator_(std::move(coordinator)),
      load_coordinator_(std::move(load_coordinator)),
      events_(std::move(events)),
      model_memory_mb_(model_memory_mb > 0 ? model_memory_mb : 4096),
      new_request_aging_threshold_(queueAgingThresholdFromEnv()),
      cancel_grace_period_(cancelGracePeriodFromEnv()) {
    if (model_id_.empty()) {
        throw std::invalid_argument("ModelRuntime requires a non-empty model id");
    }
    if (!backend_) {
        throw std::invalid_argument("ModelRuntime requires a model backend");
    }
    if (!orchestrator_) {
        throw std::invalid_argument("ModelRuntime requires an orchestrator");
    }
    if (!memory_coordinator_) {
        throw std::invalid_argument(
            "ModelRuntime requires a conversation memory coordinator");
    }
    if (!load_coordinator_) {
        throw std::invalid_argument(
            "ModelRuntime requires a model load coordinator");
    }
    post_turn_worker_ = std::make_unique<PostTurnWorker>(
        model_id_,
        *backend_,
        orchestrator_,
        memory_coordinator_,
        [this]() {
            finishPostTurn();
        });
}

ModelRuntime::~ModelRuntime() {
    stop(false);
    stopCancelWatchdog();
}

void ModelRuntime::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
        return;
    }

    stop_requested_ = false;
    force_stop_ = false;
    {
        std::lock_guard<std::mutex> watchdog_lock(cancel_watchdog_mutex_);
        cancel_watchdog_shutdown_ = false;
    }
    started_ = true;
    post_turn_worker_->start();
    try {
        executor_thread_ = std::thread(&ModelRuntime::executorLoop, this);
    } catch (...) {
        started_ = false;
        post_turn_worker_->stop(true);
        throw;
    }
    LOG_INFO("[ModelRuntime] Started executor: model=" << model_id_
             << " cancel_grace_ms=" << cancel_grace_period_.count());
}

void ModelRuntime::enqueue(GenerativeJobPtr job) {
    const std::string job_id = job ? job->job_id : std::string("<null>");
    queue_.push(std::move(job));
    LOG_INFO("[ModelRuntime] Enqueued job: model=" << model_id_
             << " job=" << job_id);
    cv_.notify_one();
}

bool ModelRuntime::activate() {
    std::vector<ModelRuntimeState> state_events;
    bool should_notify = false;

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
            state_ == ModelRuntimeState::PostTurn ||
            state_ == ModelRuntimeState::Draining) {
            drain_mode_ = DrainMode::None;
            if (state_ == ModelRuntimeState::Draining && !running_job_) {
                setStateLocked(ModelRuntimeState::Idle, state_events);
                should_notify = true;
            }
        } else {
            LOG_WARN("[ModelRuntime] Cold activation requires a load reservation: model="
                     << model_id_ << " state=" << stateToString(state_));
            return false;
        }
    }

    notifyStateChanges(state_events);
    if (should_notify) {
        cv_.notify_one();
    }
    return true;
}

bool ModelRuntime::activate(
    ModelLoadCoordinator::LoadReservation reservation) {
    if (!reservation) {
        return false;
    }

    std::vector<ModelRuntimeState> state_events;
    bool should_notify = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == ModelRuntimeState::Stopped || stop_requested_) {
            LOG_WARN("[ModelRuntime] Reserved activation ignored because runtime is stopping: model="
                     << model_id_ << " state=" << stateToString(state_));
            return false;
        }

        if (state_ == ModelRuntimeState::NotResident ||
            state_ == ModelRuntimeState::Failed) {
            pending_load_reservation_ = std::move(reservation);
            activation_requested_ = true;
            drain_mode_ = DrainMode::None;
            backend_healthy_ = false;
            setStateLocked(ModelRuntimeState::Loading, state_events);
            should_notify = true;
        } else if (state_ == ModelRuntimeState::Loading ||
                   state_ == ModelRuntimeState::Idle ||
                   state_ == ModelRuntimeState::Running ||
                   state_ == ModelRuntimeState::PostTurn ||
                   state_ == ModelRuntimeState::Draining) {
            drain_mode_ = DrainMode::None;
            if (state_ == ModelRuntimeState::Draining && !running_job_) {
                setStateLocked(ModelRuntimeState::Idle, state_events);
                should_notify = true;
            }
        } else {
            return false;
        }
    }

    notifyStateChanges(state_events);
    if (should_notify) {
        cv_.notify_one();
    }
    return true;
}

CancelResult ModelRuntime::cancel(const std::string& job_id) {
    if (job_id.empty()) {
        return CancelResult{
            CancelStatus::NOT_FOUND,
            job_id,
            "Cancel request is missing job_id"};
    }

    if (GenerativeJobPtr queued = queue_.cancel(job_id)) {
        LOG_INFO("[ModelRuntime] Cancelled queued job: model=" << model_id_
                 << " job=" << job_id);
        notifyCancelled(queued);
        return CancelResult{
            CancelStatus::QUEUED_CANCELLED,
            job_id,
            "Queued scheduler job cancelled"};
    }

    GenerativeJobPtr running;
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
             << " grace_ms=" << cancel_grace_period_.count());
    notifyCancelled(running);
    try {
        armCancelWatchdog(job_id);
    } catch (const std::exception& e) {
        LOG_WARN("[ModelRuntime] Failed to arm cancel grace watchdog: model="
                 << model_id_ << " job=" << job_id
                 << " error=" << e.what());
    } catch (...) {
        LOG_WARN("[ModelRuntime] Failed to arm cancel grace watchdog: model="
                 << model_id_ << " job=" << job_id
                 << " error=<unknown>");
    }

    return CancelResult{
        CancelStatus::RUNNING_CANCELLED,
        job_id,
        "Running scheduler job soft-cancelled and grace watchdog armed"};
}

void ModelRuntime::requestDrain() {
    std::vector<ModelRuntimeState> state_events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == ModelRuntimeState::Stopped || stop_requested_) {
            return;
        }

        if (state_ == ModelRuntimeState::Loading) {
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

void ModelRuntime::failQueued(const GenAIException& error) {
    while (GenerativeJobPtr job = queue_.pop()) {
        notifyError(job, error);
    }
}

void ModelRuntime::stop(bool force) {
    stopCancelWatchdog();

    ModelLoadCoordinator::LoadReservation pending_load_reservation;
    std::string running_job_id;
    bool post_turn_active = false;
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
            pending_load_reservation =
                std::move(pending_load_reservation_);
            drain_mode_ = DrainMode::None;

            if (force && running_job_) {
                running_job_id = running_job_->job_id;
                running_job_->cancel();
            }
            post_turn_active = force &&
                               state_ == ModelRuntimeState::PostTurn;

            should_join = executor_thread_.joinable() &&
                          executor_thread_.get_id() != std::this_thread::get_id();
        }
    }

    pending_load_reservation.release();
    notifyStateChanges(state_events);

    if (!was_started) {
        return;
    }

    if (force && (!running_job_id.empty() || post_turn_active)) {
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
        bool should_reload_unhealthy = false;
        ModelLoadCoordinator::LoadReservation initial_load_reservation;
        GenerativeJobPtr job;
        std::vector<ModelRuntimeState> state_events;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() {
                return stop_requested_ ||
                       activation_requested_ ||
                       (drain_mode_ != DrainMode::None &&
                        (state_ == ModelRuntimeState::Idle ||
                         state_ == ModelRuntimeState::Draining)) ||
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
                state_ == ModelRuntimeState::Loading) {
                activation_requested_ = false;
                initial_load_reservation =
                    std::move(pending_load_reservation_);
                should_load = true;
            } else if (drain_mode_ == DrainMode::Normal &&
                       (state_ == ModelRuntimeState::Idle ||
                        state_ == ModelRuntimeState::Draining)) {
                drain_mode_ = DrainMode::None;
                setStateLocked(ModelRuntimeState::Evicting, state_events);
                should_unload = true;
            } else if (state_ == ModelRuntimeState::Idle) {
                if (!queue_.empty() &&
                    (!backend_healthy_ || !backend_ || !backend_->isHealthy())) {
                    backend_healthy_ = false;
                    should_reload_unhealthy = true;
                    setStateLocked(ModelRuntimeState::Evicting, state_events);
                } else {
                    promoteAgedJobs();
                    job = queue_.pop();
                    if (job) {
                        running_job_ = job;
                        setStateLocked(ModelRuntimeState::Running, state_events);
                    }
                }
            }
        }

        notifyStateChanges(state_events);

        if (should_reload_unhealthy) {
            LOG_WARN("[ModelRuntime] Backend unhealthy with queued work while idle;"
                     " forcing reload: model=" << model_id_);
            {
                auto reclaim = load_coordinator_->beginReclaim(model_id_);
                unloadBackend(/*force=*/true);
                waitForDspMemoryReclaim(model_id_, stop_requested_);
            }
            std::vector<ModelRuntimeState> reload_events;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!stop_requested_) {
                    setStateLocked(ModelRuntimeState::Failed, reload_events);
                }
            }
            notifyStateChanges(reload_events);
            continue;
        }

        if (should_load) {
            constexpr size_t kMaxLoadAttempts = 2;
            for (size_t attempt = 1; attempt <= kMaxLoadAttempts; ++attempt) {
                ModelLoadCoordinator::LoadReservation reservation;
                if (attempt == 1) {
                    reservation = std::move(initial_load_reservation);
                } else {
                    try {
                        reservation = load_coordinator_->acquire(
                            model_id_, model_memory_mb_);
                    } catch (const GenAIException& error) {
                        if (!stop_requested_) {
                            failQueued(error);
                        }
                        break;
                    }
                }

                if (!reservation) {
                    if (!stop_requested_) {
                        std::vector<ModelRuntimeState> failure_events;
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            setStateLocked(
                                ModelRuntimeState::Failed,
                                failure_events);
                        }
                        notifyStateChanges(failure_events);
                        failQueued(loadFailureError(
                            model_id_,
                            std::runtime_error(
                                "Model activation lost its load reservation")));
                    }
                    break;
                }

                bool load_cancelled = false;
                std::vector<ModelRuntimeState> loading_events;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    load_cancelled = stop_requested_;
                    if (!load_cancelled && attempt > 1) {
                        setStateLocked(
                            ModelRuntimeState::Loading,
                            loading_events);
                    }
                }
                if (load_cancelled) {
                    reservation.release();
                    break;
                }
                notifyStateChanges(loading_events);

                auto handle_load_failure =
                    [this, attempt, &reservation](const GenAIException& error) {
                        std::vector<ModelRuntimeState> eviction_events;
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            backend_healthy_ = false;
                            if (!stop_requested_) {
                                setStateLocked(
                                    ModelRuntimeState::Evicting,
                                    eviction_events);
                            }
                        }
                        notifyStateChanges(eviction_events);

                        {
                            auto reclaim = load_coordinator_->beginReclaim(
                                model_id_,
                                std::move(reservation));
                            unloadBackend(/*force=*/true);
                            waitForDspMemoryReclaim(
                                model_id_, stop_requested_);
                        }

                        std::vector<ModelRuntimeState> recovery_events;
                        bool retry = false;
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            if (!stop_requested_ && attempt < kMaxLoadAttempts) {
                                retry = true;
                            } else if (!stop_requested_) {
                                setStateLocked(
                                    ModelRuntimeState::Failed,
                                    recovery_events);
                            }
                        }
                        notifyStateChanges(recovery_events);
                        if (!retry && !stop_requested_) {
                            failQueued(error);
                        }
                        return retry;
                    };

                try {
                    LOG_INFO("[ModelRuntime] Loading backend: model="
                             << model_id_ << " attempt=" << attempt
                             << "/" << kMaxLoadAttempts);
                    backend_->loadModel(model_id_);
                    reservation.release();
                    LOG_INFO("[ModelRuntime] Backend loaded: model=" << model_id_);
                    qai_forge::writeUseLock(model_id_);
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
                    break;
                } catch (const GenAIException& error) {
                    LOG_ERROR("[ModelRuntime] Backend load failed: model="
                              << model_id_ << " attempt=" << attempt
                              << "/" << kMaxLoadAttempts
                              << " status=" << error.http_status
                              << " message=\"" << error.message << "\"");
                    if (!handle_load_failure(error)) {
                        break;
                    }
                } catch (const std::exception& error) {
                    LOG_ERROR("[ModelRuntime] Backend load failed: model="
                              << model_id_ << " attempt=" << attempt
                              << "/" << kMaxLoadAttempts
                              << " message=\"" << error.what() << "\"");
                    if (!handle_load_failure(
                            loadFailureError(model_id_, error))) {
                        break;
                    }
                }
            }
            continue;
        }

        if (should_unload) {
            LOG_INFO("[ModelRuntime] Unloading backend: model=" << model_id_);
            {
                auto reclaim = load_coordinator_->beginReclaim(model_id_);
                unloadBackend(false);
                waitForDspMemoryReclaim(model_id_, stop_requested_);
            }
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
            invalidateCancelWatchdog();
            notifyStateChanges(cancel_events);
            continue;
        }

        bool recover_backend = false;
        bool post_turn_started = false;
        try {
            LOG_INFO("[ModelRuntime] Running job: model=" << model_id_
                     << " job=" << job->job_id
                     << " priority=" << static_cast<int>(job->priority)
                     << " stream="
                     << (job->callbacks.on_token ? "true" : "false"));
            post_turn_started = runJob(*job);
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

        bool cancel_force_killed = false;
        std::vector<ModelRuntimeState> complete_events;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancel_force_killed = job->isCancelled() && !backend_healthy_;
            running_job_.reset();
            if (recover_backend || cancel_force_killed) {
                backend_healthy_ = false;
                if (!stop_requested_) {
                    setStateLocked(ModelRuntimeState::Evicting, complete_events);
                }
            } else if (!stop_requested_ && !post_turn_started) {
                setStateLocked(
                    drain_mode_ != DrainMode::None ? ModelRuntimeState::Draining
                                                   : ModelRuntimeState::Idle,
                    complete_events);
            }
        }

        notifyStateChanges(complete_events);
        invalidateCancelWatchdog();

        if (recover_backend || cancel_force_killed) {
            LOG_WARN("[ModelRuntime] Recovering backend after "
                     << (cancel_force_killed
                         ? "cancel watchdog force-kill"
                         : "execution failure")
                     << ": model=" << model_id_ << " job=" << job->job_id);
            {
                auto reclaim = load_coordinator_->beginReclaim(model_id_);
                unloadBackend(true);
                waitForDspMemoryReclaim(model_id_, stop_requested_);
            }
            std::vector<ModelRuntimeState> failure_events;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!stop_requested_) {
                    setStateLocked(ModelRuntimeState::Failed, failure_events);
                }
            }
            notifyStateChanges(failure_events);
        }

        cv_.notify_one();
    }

    bool force_unload = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        force_unload = force_stop_;
    }
    post_turn_worker_->stop(force_unload);

    std::vector<ModelRuntimeState> exit_events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (isResidentState(state_)) {
            setStateLocked(ModelRuntimeState::Evicting, exit_events);
        }
    }

    notifyStateChanges(exit_events);
    {
        auto reclaim = load_coordinator_->beginReclaim(model_id_);
        unloadBackend(force_unload);
        waitForDspMemoryReclaim(model_id_, stop_requested_);
    }
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

bool ModelRuntime::runJob(GenerativeJob& job) {
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

    auto notify_cancelled = [&job]() {
        if (job.markCancellationNotified() && job.callbacks.on_cancelled) {
            job.callbacks.on_cancelled();
        }
    };

    if (job.isCancelled()) {
        notify_cancelled();
        return false;
    }
    std::string finish_reason = "stop";
    std::string response_id = job.session_id;
    const auto original_on_token = job.callbacks.on_token;
    const bool streaming = static_cast<bool>(original_on_token);
    if (streaming) {
        job.callbacks.on_token =
            [&job,
             &finish_reason,
             &response_id,
             original_on_token](const StreamChunk& chunk) {
                if (chunk.finish_reason.has_value()) {
                    finish_reason = chunk.finish_reason.value();
                }
                if (!chunk.id.empty()) {
                    response_id = chunk.id;
                }
                if (!job.isCancelled()) {
                    original_on_token(chunk);
                }
            };
    }

    try {
        if (streaming) {
            LOG_INFO("[ModelRuntime] Streaming execution started: job="
                     << job.job_id << " model=" << model_id_);
        } else {
            LOG_INFO("[ModelRuntime] Blocking execution started: job="
                     << job.job_id << " model=" << model_id_);
        }

        StandardResponse response = orchestrator_->execute(job, *backend_);
        job.callbacks.on_token = original_on_token;

        if (job.isCancelled()) {
            notify_cancelled();
            return false;
        }

        const bool post_turn_started = beginPostTurn(job, response);

        if (job.callbacks.on_complete) {
            if (response.id.empty()) {
                response.id = response_id;
            }
            if (response.finish_reason.empty()) {
                response.finish_reason = finish_reason;
            }
            try {
                job.callbacks.on_complete(response);
            } catch (...) {
                LOG_WARN("[ModelRuntime] Completion callback threw: job="
                         << job.job_id << " model=" << model_id_);
            }
        }

        if (streaming) {
            LOG_INFO("[ModelRuntime] Streaming execution completed: job="
                     << job.job_id << " model=" << model_id_
                     << " finish_reason=" << finish_reason);
        } else {
            LOG_INFO("[ModelRuntime] Blocking execution completed: job="
                     << job.job_id << " model=" << model_id_
                     << " finish_reason=" << response.finish_reason);
        }
        return post_turn_started;
    } catch (...) {
        job.callbacks.on_token = original_on_token;
        if (job.isCancelled()) {
            notify_cancelled();
            return false;
        }
        LOG_WARN("[ModelRuntime] Execution threw: job=" << job.job_id
                 << " model=" << model_id_);
        throw;
    }
}

bool ModelRuntime::beginPostTurn(
    GenerativeJob& job,
    const StandardResponse& response) {
    std::optional<PostTurnTask> task;
    try {
        task = orchestrator_->createPostTurnTask(job, response);
    } catch (const std::exception& error) {
        LOG_WARN("[ModelRuntime] Failed to prepare post-turn work: model="
                 << model_id_ << " session=" << job.session_id
                 << " message=\"" << error.what() << "\"");
        return false;
    } catch (...) {
        LOG_WARN("[ModelRuntime] Failed to prepare post-turn work: model="
                 << model_id_ << " session=" << job.session_id
                 << " message=<unknown>");
        return false;
    }
    if (!task.has_value()) {
        return false;
    }
    if (task->input.conversation_memory_key.empty()) {
        task->input.conversation_memory_key = task->input.session_id;
    }
    const std::string conversation_memory_key =
        task->input.conversation_memory_key;
    if (!memory_coordinator_->beginPostTurn(conversation_memory_key)) {
        LOG_WARN("[ModelRuntime] Post-turn already pending; using committed memory: model="
                 << model_id_ << " session=" << task->input.session_id);
        return false;
    }
    const std::string session_id = task->input.session_id;

    std::vector<ModelRuntimeState> state_events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_requested_) {
            memory_coordinator_->publish(
                conversation_memory_key,
                MemoryReadyResult::Status::Cancelled);
            return false;
        }
        running_job_.reset();
        setStateLocked(ModelRuntimeState::PostTurn, state_events);
    }
    notifyStateChanges(state_events);

    try {
        if (post_turn_worker_->enqueue(std::move(task.value()))) {
            return true;
        }
    } catch (const std::exception& error) {
        LOG_WARN("[ModelRuntime] Failed to enqueue post-turn work: model="
                 << model_id_ << " session=" << session_id
                 << " message=\"" << error.what() << "\"");
    } catch (...) {
        LOG_WARN("[ModelRuntime] Failed to enqueue post-turn work: model="
                 << model_id_ << " session=" << session_id
                 << " message=<unknown>");
    }

    memory_coordinator_->publish(
        conversation_memory_key,
        MemoryReadyResult::Status::Failed);
    finishPostTurn();
    return false;
}

void ModelRuntime::finishPostTurn() {
    std::vector<ModelRuntimeState> state_events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != ModelRuntimeState::PostTurn) {
            return;
        }
        setStateLocked(
            stop_requested_ || drain_mode_ != DrainMode::None
                ? ModelRuntimeState::Draining
                : ModelRuntimeState::Idle,
            state_events);
    }
    notifyStateChanges(state_events);
    cv_.notify_one();
}

void ModelRuntime::armCancelWatchdog(const std::string& job_id) {
    std::thread previous_thread;
    {
        std::lock_guard<std::mutex> lock(cancel_watchdog_mutex_);
        if (cancel_watchdog_shutdown_) {
            LOG_WARN("[ModelRuntime] Cancel grace watchdog skipped; runtime is "
                     "stopping: model=" << model_id_ << " job=" << job_id);
            return;
        }
        ++cancel_watchdog_generation_;
        if (cancel_watchdog_thread_.joinable()) {
            previous_thread = std::move(cancel_watchdog_thread_);
        }
    }

    cancel_watchdog_cv_.notify_all();
    if (previous_thread.joinable()) {
        previous_thread.join();
    }

    {
        std::lock_guard<std::mutex> lock(cancel_watchdog_mutex_);
        if (cancel_watchdog_shutdown_) {
            LOG_WARN("[ModelRuntime] Cancel grace watchdog skipped after join; "
                     "runtime is stopping: model=" << model_id_
                     << " job=" << job_id);
            return;
        }
        const std::uint64_t generation = ++cancel_watchdog_generation_;
        cancel_watchdog_thread_ = std::thread(
            &ModelRuntime::cancelWatchdogLoop, this, job_id, generation);
    }

    LOG_WARN("[ModelRuntime] Cancel grace watchdog armed: model=" << model_id_
             << " job=" << job_id
             << " grace_ms=" << cancel_grace_period_.count());
}

void ModelRuntime::invalidateCancelWatchdog() {
    {
        std::lock_guard<std::mutex> lock(cancel_watchdog_mutex_);
        ++cancel_watchdog_generation_;
    }
    cancel_watchdog_cv_.notify_all();
}

void ModelRuntime::stopCancelWatchdog() {
    std::thread thread_to_join;
    {
        std::lock_guard<std::mutex> lock(cancel_watchdog_mutex_);
        cancel_watchdog_shutdown_ = true;
        ++cancel_watchdog_generation_;
        if (cancel_watchdog_thread_.joinable()) {
            thread_to_join = std::move(cancel_watchdog_thread_);
        }
    }

    cancel_watchdog_cv_.notify_all();
    if (thread_to_join.joinable()) {
        thread_to_join.join();
    }
}

void ModelRuntime::cancelWatchdogLoop(std::string job_id,
                                      std::uint64_t generation) {
    {
        std::unique_lock<std::mutex> lock(cancel_watchdog_mutex_);
        const bool cancelled = cancel_watchdog_cv_.wait_for(
            lock,
            cancel_grace_period_,
            [this, generation]() {
                return cancel_watchdog_shutdown_ ||
                       generation != cancel_watchdog_generation_;
            });
        if (cancelled) {
            return;
        }
    }

    handleCancelWatchdogTimeout(job_id);
}

void ModelRuntime::handleCancelWatchdogTimeout(const std::string& job_id) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!running_job_ ||
        running_job_->job_id != job_id ||
        !running_job_->isCancelled() ||
        !backend_healthy_) {
        return;
    }

    LOG_WARN("[ModelRuntime] Cancel grace expired; force-killing worker: model="
             << model_id_ << " job=" << job_id
             << " grace_ms=" << cancel_grace_period_.count());

    bool killed = false;
    try {
        killed = backend_ && backend_->forceKillActiveWorker();
    } catch (const std::exception& e) {
        LOG_WARN("[ModelRuntime] Worker force-kill failed: model=" << model_id_
                 << " job=" << job_id << " error=" << e.what());
    } catch (...) {
        LOG_WARN("[ModelRuntime] Worker force-kill failed: model=" << model_id_
                 << " job=" << job_id << " error=<unknown>");
    }

    if (killed) {
        backend_healthy_ = false;
    }
}

void ModelRuntime::unloadBackend(bool force) {
    try {
        backend_->unloadModel(force);
        qai_forge::removeUseLock(model_id_);
        LOG_INFO("[ModelRuntime] Backend unloaded: model=" << model_id_
                 << " force=" << (force ? "true" : "false"));
    } catch (...) {
        LOG_WARN("[ModelRuntime] Backend unload threw and was suppressed; retaining use lock: model="
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
           state == ModelRuntimeState::PostTurn ||
           state == ModelRuntimeState::Draining ||
           state == ModelRuntimeState::Evicting;
}

void ModelRuntime::notifyCancelled(const GenerativeJobPtr& job) {
    if (!job || !job->markCancellationNotified() ||
        !job->callbacks.on_cancelled) {
        return;
    }

    try {
        job->callbacks.on_cancelled();
    } catch (...) {
    }
}

void ModelRuntime::notifyError(const GenerativeJobPtr& job,
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
