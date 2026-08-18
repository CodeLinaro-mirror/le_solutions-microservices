// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/scheduler/PredictiveModelPool.h"
#include "qai_forge/backend/BackendFactory.h"
#include "qai_forge/backend/IInferenceBackend.h"
#include "qai_forge/InternalDTOs.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/managers/SystemResourceManager.h"
#include "qai_forge/utils/Logger.h"
#include <algorithm>
#include <stdexcept>

namespace scheduler {

PredictiveModelPool::PredictiveModelPool(
    WarmModelPoolConfig config,
    PredictiveBackendFactory factory,
    ModelRuntimeEvents runtime_events,
    std::shared_ptr<ModelLoadCoordinator> load_coordinator)
    : config_(std::move(config))
    , factory_(std::move(factory))
    , runtime_events_(std::move(runtime_events))
    , load_coordinator_(std::move(load_coordinator))
{
    // Use default factory if none provided
    if (!factory_) {
        factory_ = [](const std::string& model_id) {
            return BackendFactory::createPredictiveBackendForModel(model_id);
        };
    }
    if (!load_coordinator_) {
        owns_load_coordinator_ = true;
        load_coordinator_ =
            std::make_shared<ModelLoadCoordinator>(config_.memory_headroom_mb);
        load_coordinator_->start();
    }

    LOG_INFO("[PredictiveModelPool] Initialized with max_active_models="
             << config_.max_active_models
             << ", idle_timeout=" << config_.idle_timeout.count() << "ms"
             << ", memory_headroom=" << config_.memory_headroom_mb << "MB");
}

PredictiveModelPool::~PredictiveModelPool() {
    try {
        stop(true);
    } catch (const std::exception& e) {
        LOG_ERROR("[PredictiveModelPool] Exception in destructor: " << e.what());
    }
}

PredictiveModelRuntime* PredictiveModelPool::reserve(
    const std::string& model_id) {
    if (model_id.empty()) {
        throw GenAIException(
            GenAIErrorCode::INVALID_REQUEST,
            "Cannot reserve a predictive runtime without model_id",
            400);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_requested_) {
        throw GenAIException(
            GenAIErrorCode::HARDWARE_UNAVAILABLE,
            "PredictiveModelPool is shutting down",
            503);
    }

    evictIdleModels();

    auto it = runtimes_.find(model_id);
    if (it == runtimes_.end()) {
        evictIfNeeded(model_id);
        it = runtimes_.find(model_id);
        if (it == runtimes_.end()) {
            RuntimeRecord& record = createRuntimeLocked(model_id);
            ++record.reservation_count;
            return record.runtime.get();
        }
    }

    ++it->second.reservation_count;
    return it->second.runtime.get();
}

SubmitResult PredictiveModelPool::submit(
    PredictiveModelRuntime* runtime,
    PredictiveJobPtr job) {
    if (!runtime || !job) {
        return SubmitResult{
            SubmitStatus::REJECTED_MODEL_NOT_FOUND,
            job ? job->job_id : std::string{},
            "Predictive submit is missing its job or reserved runtime",
        };
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_requested_) {
            return SubmitResult{
                SubmitStatus::REJECTED_SHUTTING_DOWN,
                job->job_id,
                "PredictiveModelPool is shutting down",
            };
        }

        auto it = runtimes_.find(job->model_id);
        if (it == runtimes_.end() || it->second.runtime.get() != runtime) {
            return SubmitResult{
                SubmitStatus::REJECTED_MODEL_NOT_FOUND,
                job->job_id,
                "Reserved predictive runtime does not match model '" +
                    job->model_id + "'",
            };
        }
        if (!runtime->enqueue(job, config_.max_queue_depth_per_model)) {
            return SubmitResult{
                SubmitStatus::REJECTED_QUEUE_FULL,
                job->job_id,
                "Predictive queue is full for model '" + job->model_id + "'",
            };
        }
    }

    return SubmitResult{
        SubmitStatus::QUEUED,
        job->job_id,
        "queued",
    };
}

void PredictiveModelPool::releaseReservation(
    PredictiveModelRuntime* runtime) noexcept {
    if (!runtime) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : runtimes_) {
        RuntimeRecord& record = entry.second;
        if (record.runtime.get() != runtime || record.reservation_count == 0) {
            continue;
        }
        --record.reservation_count;
        break;
    }
}

ModelPoolSnapshot PredictiveModelPool::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);

    ModelPoolSnapshot snap;
    snap.max_active_models = config_.max_active_models;
    snap.runtime_count = runtimes_.size();
    snap.active_reserved_models = activeModelCount();

    for (const auto& [model_id, record] : runtimes_) {
        ModelPoolRuntimeSnapshot runtime_snap;
        runtime_snap.model_id = model_id;
        const ModelRuntimeSnapshot detailed = record.runtime->snapshot();
        runtime_snap.state = detailed.state;
        runtime_snap.active_reserved =
            record.reservation_count > 0 ||
            detailed.queue.total() > 0 ||
            detailed.has_running_job ||
            (runtime_snap.state != ModelRuntimeState::NotResident &&
             runtime_snap.state != ModelRuntimeState::Stopped &&
             runtime_snap.state != ModelRuntimeState::Failed);
        runtime_snap.reservation_count = record.reservation_count;
        runtime_snap.queue = detailed.queue;
        runtime_snap.has_running_job = detailed.has_running_job;
        runtime_snap.running_job_id = detailed.running_job_id;
        runtime_snap.last_used_at = record.runtime->lastUsedAt();
        if (runtime_snap.state == ModelRuntimeState::Idle) {
            runtime_snap.idle_since = runtime_snap.last_used_at;
        }
        runtime_snap.healthy = detailed.healthy;

        snap.runtimes.push_back(runtime_snap);
    }

    return snap;
}

void PredictiveModelPool::stop(bool force) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (shutdown_requested_) {
        return;
    }

    shutdown_requested_ = true;
    if (owns_load_coordinator_) {
        load_coordinator_->shutdown();
    }
    LOG_INFO("[PredictiveModelPool] Stopping pool (force=" << force
             << ", active_models=" << runtimes_.size() << ")");

    // Stop all runtimes
    for (auto& [model_id, record] : runtimes_) {
        try {
            if (record.runtime) {
                record.runtime->stop(force);
            }
        } catch (const std::exception& e) {
            LOG_ERROR("[PredictiveModelPool] Error stopping runtime for model '"
                      << model_id << "': " << e.what());
        }
    }

    runtimes_.clear();
    LOG_INFO("[PredictiveModelPool] Pool stopped");
}

// ─────────────────────────────────────────────────────────────────────────────
// Private methods
// ─────────────────────────────────────────────────────────────────────────────

void PredictiveModelPool::evictIfNeeded(const std::string& model_id_to_load) {
    const size_t active_count = activeModelCount();
    const long available_mb = availableMemoryMb();
    const long required_mb = modelMemoryMb(model_id_to_load);
    const bool at_model_limit =
        active_count >= config_.max_active_models;
    const bool memory_available =
        available_mb >= required_mb + config_.memory_headroom_mb;

    if (!at_model_limit && memory_available) {
        return;
    }

    if (!at_model_limit &&
        load_coordinator_->isMemoryTransitionActive()) {
        LOG_INFO("[PredictiveModelPool] Shared memory is changing; "
                 "deferring admission: model=" << model_id_to_load);
        return;
    }

    if (at_model_limit) {
        LOG_INFO("[PredictiveModelPool] At max_active_models ("
                 << config_.max_active_models << ") — evicting LRU model");
    } else {
        LOG_INFO("[PredictiveModelPool] Insufficient memory (available="
                 << available_mb << "MB, required=" << required_mb
                 << "MB + headroom=" << config_.memory_headroom_mb
                 << "MB) — evicting models");
    }

    std::vector<std::pair<
        std::string,
        std::chrono::steady_clock::time_point>> candidates;
    for (const auto& [model_id, record] : runtimes_) {
        const ModelRuntimeSnapshot snapshot = record.runtime->snapshot();
        if (snapshot.state == ModelRuntimeState::Idle &&
            snapshot.queue.total() == 0 &&
            record.reservation_count == 0) {
            candidates.emplace_back(model_id, record.runtime->lastUsedAt());
        }
    }

    if (candidates.empty()) {
        throw GenAIException(
            GenAIErrorCode::INSUFFICIENT_MEMORY,
            "Cannot load model '" + model_id_to_load +
                "': no idle predictive model is eligible for eviction",
            503);
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) {
                  return a.second < b.second;
              });

    const std::string victim_id = candidates.front().first;
    LOG_INFO("[PredictiveModelPool] Evicting model '" << victim_id
             << "' to make room for '" << model_id_to_load << "'");

    auto it = runtimes_.find(victim_id);
    if (it != runtimes_.end()) {
        try {
            it->second.runtime->stop(false);
        } catch (const std::exception& e) {
            LOG_ERROR("[PredictiveModelPool] Error stopping runtime during eviction: "
                      << e.what());
        }
        runtimes_.erase(it);
        evictIfNeeded(model_id_to_load);
    }
}

void PredictiveModelPool::evictIdleModels() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::string> to_evict;

    for (auto& [model_id, record] : runtimes_) {
        const ModelRuntimeSnapshot snapshot = record.runtime->snapshot();
        if (snapshot.state != ModelRuntimeState::Idle ||
            snapshot.queue.total() > 0) {
            continue;
        }

        if (record.reservation_count > 0) {
            continue;
        }

        const auto idle_duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - record.runtime->lastUsedAt());

        if (idle_duration >= config_.idle_timeout) {
            LOG_INFO("[PredictiveModelPool] Model '" << model_id
                     << "' idle for " << idle_duration.count()
                     << "ms (threshold: " << config_.idle_timeout.count()
                     << "ms) — marking for eviction");
            to_evict.push_back(model_id);
        }
    }

    // Evict idle models
    for (const auto& model_id : to_evict) {
        auto it = runtimes_.find(model_id);
        if (it != runtimes_.end()) {
            try {
                it->second.runtime->stop(false);
            } catch (const std::exception& e) {
                LOG_ERROR("[PredictiveModelPool] Error stopping idle runtime: "
                          << e.what());
            }
            runtimes_.erase(it);
        }
    }
}

PredictiveModelPool::RuntimeRecord& PredictiveModelPool::createRuntimeLocked(
    const std::string& model_id) {

    LOG_INFO("[PredictiveModelPool] Creating runtime for model '" << model_id << "'");

    // Create backend instance
    std::unique_ptr<IInferenceBackend> backend;
    try {
        backend = factory_(model_id);
    } catch (const std::exception& e) {
        LOG_ERROR("[PredictiveModelPool] Failed to create backend for model '"
                  << model_id << "': " << e.what());
        throw;
    }

    // The pool queries runtime state directly instead of mirroring it through
    // callbacks, which avoids callback re-entry while the pool mutex is held.
    auto runtime = std::make_unique<PredictiveModelRuntime>(
        model_id,
        std::move(backend),
        BackendFactory::createPredictiveOrchestrator(),
        load_coordinator_,
        modelMemoryMb(model_id),
        runtime_events_);

    runtime->start();

    // Insert into map
    RuntimeRecord record;
    record.runtime = std::move(runtime);

    auto [it, inserted] = runtimes_.emplace(model_id, std::move(record));
    if (!inserted) {
        throw GenAIException(
            GenAIErrorCode::INTERNAL_ERROR,
            "Runtime for model '" + model_id + "' already exists",
            500);
    }

    LOG_DEBUG("[PredictiveModelPool] Runtime created for model '" << model_id << "'");
    return it->second;
}

size_t PredictiveModelPool::activeModelCount() const {
    size_t count = 0;
    for (const auto& [model_id, record] : runtimes_) {
        const ModelRuntimeSnapshot snapshot = record.runtime->snapshot();
        if (record.reservation_count > 0 ||
            snapshot.queue.total() > 0 ||
            snapshot.has_running_job ||
            (snapshot.state != ModelRuntimeState::NotResident &&
             snapshot.state != ModelRuntimeState::Stopped &&
             snapshot.state != ModelRuntimeState::Failed)) {
            ++count;
        }
    }
    return count;
}

long PredictiveModelPool::availableMemoryMb() const {
    return load_coordinator_->availableMemoryMb();
}

long PredictiveModelPool::modelMemoryMb(const std::string& model_id) const {
    const auto* config =
        ModelConfigManager::getInstance().getModelConfig(model_id);
    if (!config) {
        LOG_WARN("[PredictiveModelPool] Model '" << model_id
                 << "' not found in config — assuming 512MB");
        return 512;
    }

    // Use memory_requirement_mb if available, otherwise estimate
    if (config->memory_requirement_mb > 0) {
        return config->memory_requirement_mb;
    }

    // Default estimate for predictive models
    return 512;
}

} // namespace scheduler
