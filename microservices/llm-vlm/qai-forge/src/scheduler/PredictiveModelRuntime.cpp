// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/scheduler/PredictiveModelRuntime.h"

#include "qai_forge/backend/IInferenceBackend.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/Logger.h"
#include "qai_forge/utils/UseLock.h"

#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <utility>

namespace scheduler {

namespace {

GenAIException stoppedError(const std::string& model_id) {
    return GenAIException(
        GenAIErrorCode::HARDWARE_UNAVAILABLE,
        "Predictive runtime for model '" + model_id + "' is shutting down",
        503);
}

GenAIException executionError(const std::string& model_id,
                              const std::exception& error) {
    return GenAIException(
        GenAIErrorCode::INFERENCE_FAILED,
        "Inference failed for model '" + model_id + "': " + error.what(),
        500);
}

void waitForDspMemoryReclaim(const std::string& model_id,
                             bool stop_requested) {
    const char* value = std::getenv("DSP_RECLAIM_DELAY_MS");
    long delay_ms = 2000;
    if (value && value[0] != '\0') {
        try {
            delay_ms = std::stol(value);
        } catch (...) {
        }
    }

    if (delay_ms <= 0 || stop_requested) {
        return;
    }

    LOG_INFO("[PredictiveModelRuntime] Waiting " << delay_ms
             << "ms after backend teardown for DSP memory reclaim: model="
             << model_id);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
}

} // namespace

PredictiveModelRuntime::PredictiveModelRuntime(
    std::string model_id,
    std::unique_ptr<IInferenceBackend> backend,
    std::shared_ptr<PredictiveOrchestrator> orchestrator,
    std::shared_ptr<ModelLoadCoordinator> load_coordinator,
    long model_memory_mb,
    ModelRuntimeEvents events)
    : model_id_(std::move(model_id)),
      backend_(std::move(backend)),
      orchestrator_(std::move(orchestrator)),
      load_coordinator_(std::move(load_coordinator)),
      model_memory_mb_(model_memory_mb > 0 ? model_memory_mb : 512),
      events_(std::move(events)) {
    if (model_id_.empty()) {
        throw std::invalid_argument(
            "PredictiveModelRuntime requires a non-empty model id");
    }
    if (!backend_) {
        throw std::invalid_argument(
            "PredictiveModelRuntime requires a backend");
    }
    if (!orchestrator_) {
        throw std::invalid_argument(
            "PredictiveModelRuntime requires an orchestrator");
    }
    if (!load_coordinator_) {
        throw std::invalid_argument(
            "PredictiveModelRuntime requires a model load coordinator");
    }
}

PredictiveModelRuntime::~PredictiveModelRuntime() {
    stop(true);
}

void PredictiveModelRuntime::start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_) {
            return;
        }
        stop_requested_ = false;
        state_ = ModelRuntimeState::NotResident;
        started_ = true;
        try {
            executor_thread_ =
                std::thread(&PredictiveModelRuntime::executorLoop, this);
        } catch (...) {
            started_ = false;
            throw;
        }
    }
    notifyStateChanged(ModelRuntimeState::NotResident);
    LOG_INFO("[PredictiveModelRuntime] Started executor: model=" << model_id_);
}

bool PredictiveModelRuntime::enqueue(PredictiveJobPtr job,
                                     size_t max_queue_depth) {
    if (!job) {
        throw std::invalid_argument(
            "PredictiveModelRuntime::enqueue received null job");
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || stop_requested_) {
            return false;
        }
        if (max_queue_depth > 0 && queue_.size() >= max_queue_depth) {
            return false;
        }
        queue_.push_back(std::move(job));
    }
    cv_.notify_one();
    return true;
}

void PredictiveModelRuntime::stop(bool force) {
    (void)force;
    std::deque<PredictiveJobPtr> queued;
    bool should_join = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ && state_ == ModelRuntimeState::Stopped) {
            return;
        }
        stop_requested_ = true;
        queued.swap(queue_);
        should_join = executor_thread_.joinable();
    }

    cv_.notify_all();
    failJobs(std::move(queued), stoppedError(model_id_));
    if (should_join) {
        executor_thread_.join();
    }

    setState(ModelRuntimeState::Evicting);
    {
        auto reclaim = load_coordinator_->beginReclaim(model_id_);
        if (unloadBackend()) {
            waitForDspMemoryReclaim(model_id_, false);
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        started_ = false;
        running_job_.reset();
    }
    setState(ModelRuntimeState::Stopped);
    LOG_INFO("[PredictiveModelRuntime] Stopped executor: model=" << model_id_);
}

ModelRuntimeSnapshot PredictiveModelRuntime::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    ModelRuntimeSnapshot snapshot;
    snapshot.model_id = model_id_;
    snapshot.state = state_;
    snapshot.queue.new_request = queue_.size();
    snapshot.has_running_job = static_cast<bool>(running_job_);
    snapshot.running_job_id = running_job_ ? running_job_->job_id : std::string{};
    snapshot.healthy = backend_healthy_;
    return snapshot;
}

ModelRuntimeState PredictiveModelRuntime::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::string PredictiveModelRuntime::modelId() const {
    return model_id_;
}

std::chrono::steady_clock::time_point
PredictiveModelRuntime::lastUsedAt() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_used_at_;
}

void PredictiveModelRuntime::executorLoop() {
    while (true) {
        PredictiveJobPtr job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() {
                return stop_requested_ || !queue_.empty();
            });
            if (stop_requested_) {
                break;
            }
            job = std::move(queue_.front());
            queue_.pop_front();
            running_job_ = job;
            last_used_at_ = std::chrono::steady_clock::now();
        }

        try {
            ensureModelLoaded();
            setState(ModelRuntimeState::Running);
            TensorInferenceResponse response =
                orchestrator_->execute(*job, *backend_);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                running_job_.reset();
                last_used_at_ = std::chrono::steady_clock::now();
            }
            setState(ModelRuntimeState::Idle);
            notifyComplete(job, response);
        } catch (const GenAIException& error) {
            const ModelRuntimeState failed_state = state();
            if (failed_state != ModelRuntimeState::Failed) {
                if (error.http_status >= 500) {
                    recoverBackend();
                } else {
                    setState(ModelRuntimeState::Idle);
                }
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                running_job_.reset();
                last_used_at_ = std::chrono::steady_clock::now();
            }
            notifyError(job, error);
        } catch (const std::exception& error) {
            recoverBackend();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                running_job_.reset();
                last_used_at_ = std::chrono::steady_clock::now();
            }
            notifyError(job, executionError(model_id_, error));
        } catch (...) {
            recoverBackend();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                running_job_.reset();
                last_used_at_ = std::chrono::steady_clock::now();
            }
            notifyError(
                job,
                GenAIException(
                    GenAIErrorCode::INFERENCE_FAILED,
                    "Inference failed for model '" + model_id_ + "'",
                    500));
        }
    }
}

void PredictiveModelRuntime::ensureModelLoaded() {
    bool needs_cleanup = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_requested_) {
            throw stoppedError(model_id_);
        }
        if (state_ == ModelRuntimeState::Idle && backend_healthy_ &&
            backend_->isHealthy()) {
            return;
        }
        needs_cleanup = backend_started_;
    }

    if (needs_cleanup) {
        setState(ModelRuntimeState::Evicting);
        auto reclaim = load_coordinator_->beginReclaim(model_id_);
        if (unloadBackend()) {
            waitForDspMemoryReclaim(model_id_, false);
        }
    }

    const ModelConfig* config =
        ModelConfigManager::getInstance().getModelConfig(model_id_);
    if (!config) {
        setState(ModelRuntimeState::Failed);
        throw GenAIException(
            GenAIErrorCode::MODEL_NOT_FOUND,
            "Model '" + model_id_ + "' not found in ModelConfigManager",
            404);
    }

    constexpr size_t kMaxLoadAttempts = 2;
    for (size_t attempt = 1; attempt <= kMaxLoadAttempts; ++attempt) {
        ModelLoadCoordinator::LoadReservation reservation;
        try {
            reservation =
                load_coordinator_->acquire(model_id_, model_memory_mb_);
        } catch (const GenAIException&) {
            setState(ModelRuntimeState::Failed);
            throw;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_requested_) {
                reservation.release();
                throw stoppedError(model_id_);
            }
        }
        setState(ModelRuntimeState::Loading);

        auto cleanup_failed_attempt = [this, &reservation]() {
            setState(ModelRuntimeState::Evicting);
            auto reclaim = load_coordinator_->beginReclaim(
                model_id_,
                std::move(reservation));
            if (unloadBackend()) {
                bool stopping = false;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    stopping = stop_requested_;
                }
                waitForDspMemoryReclaim(model_id_, stopping);
            }
        };

        try {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                backend_started_ = true;
            }
            LOG_INFO("[PredictiveModelRuntime] Loading backend: model="
                     << model_id_ << " attempt=" << attempt
                     << "/" << kMaxLoadAttempts);
            backend_->initialize(model_id_, config->config_file);
            if (!backend_->isHealthy()) {
                throw std::runtime_error(
                    "Backend health check failed after initialization");
            }

            reservation.release();
            qai_forge::writeUseLock(model_id_);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                use_lock_written_ = true;
                backend_healthy_ = true;
                last_used_at_ = std::chrono::steady_clock::now();
            }
            setState(ModelRuntimeState::Idle);
            LOG_INFO("[PredictiveModelRuntime] Backend loaded: model="
                     << model_id_ << " backend=" << backend_->name());
            return;
        } catch (const GenAIException&) {
            cleanup_failed_attempt();
            if (attempt == kMaxLoadAttempts) {
                setState(ModelRuntimeState::Failed);
                throw;
            }
        } catch (const std::exception& error) {
            cleanup_failed_attempt();
            if (attempt == kMaxLoadAttempts) {
                setState(ModelRuntimeState::Failed);
                throw GenAIException(
                    GenAIErrorCode::INTERNAL_ERROR,
                    "Failed to load model '" + model_id_ + "': " + error.what(),
                    500);
            }
        }
    }
}

bool PredictiveModelRuntime::unloadBackend() {
    bool should_unload = false;
    bool remove_use_lock = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        should_unload = backend_started_;
        remove_use_lock = use_lock_written_;
        backend_started_ = false;
        use_lock_written_ = false;
        backend_healthy_ = false;
    }
    if (!should_unload) {
        return false;
    }

    try {
        backend_->shutdown();
    } catch (const std::exception& error) {
        LOG_ERROR("[PredictiveModelRuntime] Backend shutdown failed: model="
                  << model_id_ << " message=\"" << error.what() << "\"");
    }
    if (remove_use_lock) {
        qai_forge::removeUseLock(model_id_);
    }
    return true;
}

void PredictiveModelRuntime::recoverBackend() {
    setState(ModelRuntimeState::Evicting);
    {
        auto reclaim = load_coordinator_->beginReclaim(model_id_);
        if (unloadBackend()) {
            bool stopping = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stopping = stop_requested_;
            }
            waitForDspMemoryReclaim(model_id_, stopping);
        }
    }
    setState(ModelRuntimeState::Failed);
}

void PredictiveModelRuntime::setState(ModelRuntimeState state) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == state) {
            return;
        }
        state_ = state;
    }
    notifyStateChanged(state);
}

void PredictiveModelRuntime::notifyStateChanged(ModelRuntimeState state) {
    if (!events_.on_state_changed) {
        return;
    }
    try {
        events_.on_state_changed(model_id_, state);
    } catch (...) {
    }
}

void PredictiveModelRuntime::failJobs(
    std::deque<PredictiveJobPtr> jobs,
    const GenAIException& error) {
    while (!jobs.empty()) {
        PredictiveJobPtr job = std::move(jobs.front());
        jobs.pop_front();
        notifyError(job, error);
    }
}

void PredictiveModelRuntime::notifyComplete(
    const PredictiveJobPtr& job,
    const TensorInferenceResponse& response) {
    if (!job || !job->callbacks.on_complete) {
        return;
    }
    try {
        job->callbacks.on_complete(response);
    } catch (...) {
    }
}

void PredictiveModelRuntime::notifyError(const PredictiveJobPtr& job,
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
