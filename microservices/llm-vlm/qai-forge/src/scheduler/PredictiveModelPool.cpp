// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/scheduler/PredictiveModelPool.h"
#include "qai_forge/scheduler/EvictionPolicy.h"
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
    PredictiveBackendFactory factory)
    : config_(std::move(config))
    , factory_(std::move(factory))
    , eviction_policy_(std::make_unique<EvictionPolicy>())
{
    // Use default factory if none provided
    if (!factory_) {
        factory_ = [](const std::string& model_id) {
            return BackendFactory::createPredictiveBackendForModel(model_id);
        };
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

TensorInferenceResponse PredictiveModelPool::infer(
    const TensorInferenceRequest& request) {

    // Get or load the model (outside the critical section for inference)
    PredictiveModelRuntime* runtime = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (shutdown_requested_) {
            throw GenAIException(
                GenAIErrorCode::INTERNAL_ERROR,
                "PredictiveModelPool is shutting down",
                503);
        }

        // Evict idle models first (proactive cleanup)
        evictIdleModels();

        // Get or create runtime for this model
        auto it = runtimes_.find(request.model);
        if (it == runtimes_.end()) {
            // Model not loaded — check if we need to evict
            evictIfNeeded(request.model);

            // Create new runtime
            auto& record = createRuntimeLocked(request.model);
            runtime = record.runtime.get();
        } else {
            runtime = it->second.runtime.get();
        }
    }

    // Run inference (outside the pool mutex — runtime has its own mutex)
    if (!runtime) {
        throw GenAIException(
            GenAIErrorCode::INTERNAL_ERROR,
            "Failed to get runtime for model '" + request.model + "'",
            500);
    }

    TensorInferenceResponse response = runtime->infer(request);

    // Update last_used_at timestamp
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = runtimes_.find(request.model);
        if (it != runtimes_.end()) {
            it->second.last_used_at = std::chrono::steady_clock::now();
            it->second.idle_since.reset();
        }
    }

    return response;
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
        runtime_snap.state = record.runtime->state();
        runtime_snap.last_used_at = record.last_used_at;
        runtime_snap.idle_since = record.idle_since;
        runtime_snap.healthy = (runtime_snap.state == ModelRuntimeState::Idle ||
                                runtime_snap.state == ModelRuntimeState::Running);

        // Get detailed snapshot from runtime
        if (record.runtime) {
            auto rt_snap = record.runtime->snapshot();
            runtime_snap.has_running_job = rt_snap.has_running_job;
            runtime_snap.healthy = rt_snap.healthy;
        }

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

PredictiveModelRuntime& PredictiveModelPool::getOrLoadModel(
    const std::string& model_id) {

    auto it = runtimes_.find(model_id);
    if (it != runtimes_.end()) {
        return *it->second.runtime;
    }

    // Model not loaded — create new runtime
    auto& record = createRuntimeLocked(model_id);
    return *record.runtime;
}

void PredictiveModelPool::evictIfNeeded(const std::string& model_id_to_load) {
    const size_t active_count = activeModelCount();

    // Check if we're at capacity
    if (active_count < config_.max_active_models) {
        return;
    }

    // Check if we have enough memory
    const long available_mb = availableMemoryMb();
    const long required_mb = modelMemoryMb(model_id_to_load);

    if (available_mb >= required_mb + config_.memory_headroom_mb) {
        // Enough memory, but at model count limit — evict LRU idle model
        LOG_INFO("[PredictiveModelPool] At max_active_models ("
                 << config_.max_active_models << ") — evicting LRU model");
    } else {
        LOG_INFO("[PredictiveModelPool] Insufficient memory (available="
                 << available_mb << "MB, required=" << required_mb
                 << "MB + headroom=" << config_.memory_headroom_mb
                 << "MB) — evicting models");
    }

    // Build candidate list for eviction
    std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> candidates;
    for (const auto& [model_id, record] : runtimes_) {
        // Only evict idle models
        if (record.runtime->state() == ModelRuntimeState::Idle) {
            candidates.emplace_back(model_id, record.last_used_at);
        }
    }

    if (candidates.empty()) {
        throw GenAIException(
            GenAIErrorCode::INSUFFICIENT_MEMORY,
            "Cannot load model '" + model_id_to_load +
                "': all models are busy (no idle models to evict)",
            503);
    }

    // Sort by last_used_at (oldest first)
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) {
                  return a.second < b.second;
              });

    // Evict the oldest idle model
    const std::string& victim_id = candidates.front().first;
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
    }
}

void PredictiveModelPool::evictIdleModels() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::string> to_evict;

    for (auto& [model_id, record] : runtimes_) {
        if (record.runtime->state() != ModelRuntimeState::Idle) {
            // Not idle right now (e.g. Running/Loading) — any previously
            // recorded idle_since is stale.
            record.idle_since.reset();
            continue;
        }

        // Update idle_since if model just became idle
        if (!record.idle_since) {
            record.idle_since = now;
            continue;
        }

        // Check if model has been idle too long
        const auto idle_duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - *record.idle_since);

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

    // No state-change callback: PredictiveModelRuntime has no executor thread,
    // so every transition happens synchronously on the caller's thread inside
    // a call the pool itself made. The pool queries live state via
    // record.runtime->state() instead of caching a mirrored copy.
    auto runtime = std::make_unique<PredictiveModelRuntime>(
        model_id, std::move(backend), ModelRuntimeEvents{});

    runtime->start();

    // Insert into map
    RuntimeRecord record;
    record.runtime = std::move(runtime);
    record.last_used_at = std::chrono::steady_clock::now();

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
        const ModelRuntimeState state = record.runtime->state();
        if (state != ModelRuntimeState::NotResident &&
            state != ModelRuntimeState::Stopped &&
            state != ModelRuntimeState::Failed) {
            ++count;
        }
    }
    return count;
}

long PredictiveModelPool::availableMemoryMb() const {
    return SystemResourceManager::getInstance().getAvailableMemoryMb();
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
