// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/scheduler/PredictiveModelRuntime.h"
#include "qai_forge/backend/IInferenceBackend.h"
#include "qai_forge/InternalDTOs.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/Logger.h"
#include <stdexcept>

namespace scheduler {

PredictiveModelRuntime::PredictiveModelRuntime(
    std::string model_id,
    std::unique_ptr<IInferenceBackend> backend,
    ModelRuntimeEvents events)
    : model_id_(std::move(model_id))
    , backend_(std::move(backend))
    , events_(std::move(events))
    , last_used_at_(std::chrono::steady_clock::now())
{
    if (!backend_) {
        throw std::invalid_argument(
            "PredictiveModelRuntime: backend cannot be null");
    }
}

PredictiveModelRuntime::~PredictiveModelRuntime() {
    try {
        stop(true);
    } catch (const std::exception& e) {
        LOG_ERROR("[PredictiveModelRuntime] Exception in destructor for model '"
                  << model_id_ << "': " << e.what());
    }
}

void PredictiveModelRuntime::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
        return;
    }
    started_ = true;
    setStateLocked(ModelRuntimeState::NotResident);
    LOG_INFO("[PredictiveModelRuntime] Started runtime for model '" << model_id_ << "'");
}

TensorInferenceResponse PredictiveModelRuntime::infer(
    const TensorInferenceRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!started_) {
        throw GenAIException(
            GenAIErrorCode::INTERNAL_ERROR,
            "PredictiveModelRuntime not started for model '" + model_id_ + "'",
            500);
    }

    if (stop_requested_) {
        throw GenAIException(
            GenAIErrorCode::INTERNAL_ERROR,
            "PredictiveModelRuntime is shutting down for model '" + model_id_ + "'",
            503);
    }

    // Load model if not yet resident, and retry a previously failed load
    // (otherwise a model stays permanently stuck in Failed after one bad
    // attempt, surfacing a misleading "not in Idle state" 503 forever).
    if (state_ == ModelRuntimeState::NotResident ||
        state_ == ModelRuntimeState::Failed) {
        loadModelLocked();
    }

    // Verify model is ready
    if (state_ != ModelRuntimeState::Idle) {
        throw GenAIException(
            GenAIErrorCode::INTERNAL_ERROR,
            "Model '" + model_id_ + "' is not in Idle state (current: " +
                std::to_string(static_cast<int>(state_)) + ")",
            503);
    }

    // Run inference
    setStateLocked(ModelRuntimeState::Running);
    last_used_at_ = std::chrono::steady_clock::now();

    TensorInferenceResponse response;
    try {
        LOG_DEBUG("[PredictiveModelRuntime] Running inference for model '"
                  << model_id_ << "' (request_id: " << request.request_id << ")");

        response = backend_->infer(request);
        backend_healthy_ = true;

        LOG_DEBUG("[PredictiveModelRuntime] Inference completed for model '"
                  << model_id_ << "' (latency: " << response.stats.latency_ms << " ms)");

    } catch (const std::exception& e) {
        backend_healthy_ = false;
        setStateLocked(ModelRuntimeState::Failed);
        LOG_ERROR("[PredictiveModelRuntime] Inference failed for model '"
                  << model_id_ << "': " << e.what());
        throw GenAIException(
            GenAIErrorCode::INFERENCE_FAILED,
            "Inference failed for model '" + model_id_ + "': " + e.what(),
            500);
    }

    setStateLocked(ModelRuntimeState::Idle);
    return response;
}

void PredictiveModelRuntime::stop(bool force) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!started_) {
        return;
    }

    stop_requested_ = true;
    LOG_INFO("[PredictiveModelRuntime] Stopping runtime for model '"
             << model_id_ << "' (force=" << force << ")");

    unloadBackend(force);
    started_ = false;
    setStateLocked(ModelRuntimeState::Stopped);
}

ModelRuntimeSnapshot PredictiveModelRuntime::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);

    ModelRuntimeSnapshot snap;
    snap.model_id = model_id_;
    snap.state = state_;
    snap.has_running_job = (state_ == ModelRuntimeState::Running);
    snap.healthy = backend_healthy_;
    // Predictive runtimes don't have queues, so queue fields remain default
    return snap;
}

ModelRuntimeState PredictiveModelRuntime::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::string PredictiveModelRuntime::modelId() const {
    return model_id_;
}

// ─────────────────────────────────────────────────────────────────────────────
// Private methods
// ─────────────────────────────────────────────────────────────────────────────

void PredictiveModelRuntime::loadModelLocked() {
    LOG_INFO("[PredictiveModelRuntime] Loading model '" << model_id_ << "'");
    setStateLocked(ModelRuntimeState::Loading);

    try {
        // Resolve model file path from ModelConfigManager
        const auto* config =
            ModelConfigManager::getInstance().getModelConfig(model_id_);
        if (!config) {
            throw GenAIException(
                GenAIErrorCode::MODEL_NOT_FOUND,
                "Model '" + model_id_ + "' not found in ModelConfigManager",
                404);
        }

        const std::string model_file = config->config_file;
        LOG_DEBUG("[PredictiveModelRuntime] Initializing backend for model '"
                  << model_id_ << "' (file: " << model_file << ")");

        backend_->initialize(model_id_, model_file);
        backend_healthy_ = backend_->isHealthy();

        if (!backend_healthy_) {
            throw std::runtime_error("Backend health check failed after initialization");
        }

        setStateLocked(ModelRuntimeState::Idle);
        LOG_INFO("[PredictiveModelRuntime] Model '" << model_id_
                 << "' loaded successfully (backend: " << backend_->name() << ")");

    } catch (const std::exception& e) {
        backend_healthy_ = false;
        setStateLocked(ModelRuntimeState::Failed);
        LOG_ERROR("[PredictiveModelRuntime] Failed to load model '"
                  << model_id_ << "': " << e.what());
        throw GenAIException(
            GenAIErrorCode::INTERNAL_ERROR,
            "Failed to load model '" + model_id_ + "': " + e.what(),
            500);
    }
}

void PredictiveModelRuntime::unloadBackend(bool force) {
    if (state_ == ModelRuntimeState::NotResident ||
        state_ == ModelRuntimeState::Stopped) {
        return;
    }

    LOG_INFO("[PredictiveModelRuntime] Unloading model '" << model_id_ << "'");
    setStateLocked(ModelRuntimeState::Evicting);

    try {
        if (backend_) {
            backend_->shutdown();
        }
        backend_healthy_ = false;
        LOG_DEBUG("[PredictiveModelRuntime] Backend shutdown completed for model '"
                  << model_id_ << "'");
    } catch (const std::exception& e) {
        LOG_ERROR("[PredictiveModelRuntime] Error during backend shutdown for model '"
                  << model_id_ << "': " << e.what());
    }
}

void PredictiveModelRuntime::setStateLocked(ModelRuntimeState new_state) {
    if (state_ == new_state) {
        return;
    }

    const auto old_state = state_;
    state_ = new_state;

    LOG_DEBUG("[PredictiveModelRuntime] State transition for model '"
              << model_id_ << "': " << static_cast<int>(old_state)
              << " -> " << static_cast<int>(new_state));

    notifyStateChanged(new_state);
}

void PredictiveModelRuntime::notifyStateChanged(ModelRuntimeState state) {
    if (events_.on_state_changed) {
        try {
            events_.on_state_changed(model_id_, state);
        } catch (const std::exception& e) {
            LOG_ERROR("[PredictiveModelRuntime] Exception in state change callback: "
                      << e.what());
        }
    }
}

} // namespace scheduler
