// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/scheduler/PredictiveModelPool.h"
#include "qai_forge/backend/BackendFactory.h"
#include "qai_forge/backend/PredictiveBackendAdapter.h"
#include "qai_forge/InternalDTOs.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/managers/SystemResourceManager.h"
#include "qai_forge/utils/Logger.h"
#include <algorithm>
#include <stdexcept>

namespace scheduler {

namespace {

RuntimeCandidateState toRoutingState(ModelRuntimeState state) {
    switch (state) {
        case ModelRuntimeState::NotResident:
            return RuntimeCandidateState::NotResident;
        case ModelRuntimeState::Loading:
            return RuntimeCandidateState::Loading;
        case ModelRuntimeState::Idle:
            return RuntimeCandidateState::Idle;
        case ModelRuntimeState::Running:
            return RuntimeCandidateState::Running;
        case ModelRuntimeState::Draining:
            return RuntimeCandidateState::Draining;
        case ModelRuntimeState::Evicting:
            return RuntimeCandidateState::Evicting;
        case ModelRuntimeState::Failed:
            return RuntimeCandidateState::Failed;
        case ModelRuntimeState::Stopped:
            return RuntimeCandidateState::Stopped;
    }
    return RuntimeCandidateState::NotResident;
}

std::shared_ptr<IRequestRoutingPolicy> defaultRoutingPolicy() {
    return std::make_shared<LeastQueueDepthRoutingPolicy>();
}

} // namespace

PredictiveModelPool::PredictiveModelPool(
    WarmModelPoolConfig config,
    PredictiveBackendFactory factory,
    ModelRuntimeEvents runtime_events,
    std::shared_ptr<ModelLoadCoordinator> load_coordinator,
    std::shared_ptr<PlacementAdmissionGateway> placement,
    std::shared_ptr<IRequestRoutingPolicy> routing_policy)
    : config_(std::move(config))
    , factory_(std::move(factory))
    , runtime_events_(std::move(runtime_events))
    , load_coordinator_(std::move(load_coordinator))
    , placement_(std::move(placement))
    , routing_policy_(routing_policy ? std::move(routing_policy)
                                     : defaultRoutingPolicy())
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
    if (!placement_) {
        placement_ = PlacementAdmissionGateway::createDefault();
    }
    if (config_.max_replicas_per_model == 0) {
        config_.max_replicas_per_model = 1;
    }

    LOG_INFO("[PredictiveModelPool] Initialized with max_active_models="
             << config_.max_active_models
             << ", idle_timeout=" << config_.idle_timeout.count() << "ms"
             << ", memory_headroom=" << config_.memory_headroom_mb << "MB"
             << ", max_replicas_per_model=" << config_.max_replicas_per_model);
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

    const auto* existing_replicas = replicasForModelLocked(model_id);
    if (!existing_replicas || existing_replicas->empty()) {
        evictIfNeeded(model_id);
    }

    RuntimeRecord& record = routeOrCreateRuntimeLocked(model_id);
    ++record.reservation_count;
    return record.runtime.get();
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

        auto it = runtimes_.find(runtime->runtimeId());
        if (it == runtimes_.end() || it->second.runtime.get() != runtime ||
            it->second.runtime->modelId() != job->model_id) {
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

    for (const auto& [runtime_id, record] : runtimes_) {
        ModelPoolRuntimeSnapshot runtime_snap;
        runtime_snap.model_id = runtime_id.model_id;
        runtime_snap.runtime_id = runtime_id;
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
    for (auto& [runtime_id, record] : runtimes_) {
        try {
            if (record.runtime) {
                record.runtime->stop(force);
            }
        } catch (const std::exception& e) {
            LOG_ERROR("[PredictiveModelPool] Error stopping runtime for model '"
                      << runtime_id.model_id << "' replica="
                      << runtime_id.replica_id << ": " << e.what());
        }
    }

    runtimes_.clear();
    replicas_by_model_.clear();
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
        qai_forge::execution::RuntimeId,
        std::chrono::steady_clock::time_point>> candidates;
    for (const auto& [runtime_id, record] : runtimes_) {
        const ModelRuntimeSnapshot snapshot = record.runtime->snapshot();
        if (snapshot.state == ModelRuntimeState::Idle &&
            snapshot.queue.total() == 0 &&
            record.reservation_count == 0) {
            candidates.emplace_back(runtime_id, record.runtime->lastUsedAt());
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

    const qai_forge::execution::RuntimeId victim_id = candidates.front().first;
    LOG_INFO("[PredictiveModelPool] Evicting model '" << victim_id.model_id
             << "' replica=" << victim_id.replica_id
             << " to make room for '" << model_id_to_load << "'");

    auto it = runtimes_.find(victim_id);
    if (it != runtimes_.end()) {
        try {
            it->second.runtime->stop(false);
        } catch (const std::exception& e) {
            LOG_ERROR("[PredictiveModelPool] Error stopping runtime during eviction: "
                      << e.what());
        }
        runtimes_.erase(it);
        auto bucket_it = replicas_by_model_.find(victim_id.model_id);
        if (bucket_it != replicas_by_model_.end()) {
            auto& bucket = bucket_it->second;
            bucket.erase(
                std::remove(bucket.begin(), bucket.end(), victim_id),
                bucket.end());
            if (bucket.empty()) {
                replicas_by_model_.erase(bucket_it);
            }
        }
        evictIfNeeded(model_id_to_load);
    }
}

void PredictiveModelPool::evictIdleModels() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<qai_forge::execution::RuntimeId> to_evict;

    for (auto& [runtime_id, record] : runtimes_) {
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
            LOG_INFO("[PredictiveModelPool] Model '" << runtime_id.model_id
                     << "' replica=" << runtime_id.replica_id
                     << " idle for " << idle_duration.count()
                     << "ms (threshold: " << config_.idle_timeout.count()
                     << "ms) — marking for eviction");
            to_evict.push_back(runtime_id);
        }
    }

    // Evict idle models
    for (const auto& runtime_id : to_evict) {
        auto it = runtimes_.find(runtime_id);
        if (it != runtimes_.end()) {
            try {
                it->second.runtime->stop(false);
            } catch (const std::exception& e) {
                LOG_ERROR("[PredictiveModelPool] Error stopping idle runtime: "
                          << e.what());
            }
            runtimes_.erase(it);
            auto bucket_it = replicas_by_model_.find(runtime_id.model_id);
            if (bucket_it != replicas_by_model_.end()) {
                auto& bucket = bucket_it->second;
                bucket.erase(
                    std::remove(bucket.begin(), bucket.end(), runtime_id),
                    bucket.end());
                if (bucket.empty()) {
                    replicas_by_model_.erase(bucket_it);
                }
            }
        }
    }
}

PredictiveModelPool::RuntimeRecord&
PredictiveModelPool::routeOrCreateRuntimeLocked(const std::string& model_id) {
    auto replicas_it = replicas_by_model_.find(model_id);
    if (replicas_it == replicas_by_model_.end() ||
        replicas_it->second.empty()) {
        return createRuntimeLocked(
            qai_forge::execution::RuntimeId{model_id, 0});
    }

    std::vector<RuntimeCandidate> candidates;
    candidates.reserve(replicas_it->second.size());
    for (const auto& runtime_id : replicas_it->second) {
        auto it = runtimes_.find(runtime_id);
        if (it == runtimes_.end()) {
            continue;
        }
        const RuntimeRecord& record = it->second;
        const ModelRuntimeSnapshot detailed = record.runtime->snapshot();
        candidates.push_back(RuntimeCandidate{
            runtime_id,
            toRoutingState(detailed.state),
            detailed.queue.total(),
            detailed.has_running_job,
            detailed.healthy,
        });
    }

    const auto chosen = routing_policy_->choose(candidates);
    if (chosen) {
        auto it = runtimes_.find(*chosen);
        if (it != runtimes_.end()) {
            return it->second;
        }
    }

    if (replicas_it->second.size() < config_.max_replicas_per_model) {
        const uint64_t next_replica_id =
            replicas_it->second.back().replica_id + 1;
        return createRuntimeLocked(
            qai_forge::execution::RuntimeId{model_id, next_replica_id});
    }

    RuntimeRecord* fallback = firstReplicaRecordLocked(model_id);
    if (fallback) {
        return *fallback;
    }

    return createRuntimeLocked(
        qai_forge::execution::RuntimeId{model_id, 0});
}

PredictiveModelPool::RuntimeRecord& PredictiveModelPool::createRuntimeLocked(
    const qai_forge::execution::RuntimeId& runtime_id) {

    LOG_INFO("[PredictiveModelPool] Creating runtime for model '"
             << runtime_id.model_id << "' replica=" << runtime_id.replica_id);

    // Create backend instance
    std::unique_ptr<PredictiveBackendAdapter> backend;
    try {
        backend = factory_(runtime_id.model_id);
    } catch (const std::exception& e) {
        LOG_ERROR("[PredictiveModelPool] Failed to create backend for model '"
                  << runtime_id.model_id << "': " << e.what());
        throw;
    }

    // The pool queries runtime state directly instead of mirroring it through
    // callbacks, which avoids callback re-entry while the pool mutex is held.
    auto runtime = std::make_unique<PredictiveModelRuntime>(
        runtime_id.model_id,
        std::move(backend),
        BackendFactory::createPredictiveOrchestrator(),
        load_coordinator_,
        modelMemoryMb(runtime_id.model_id),
        placement_,
        runtime_events_,
        runtime_id.replica_id);

    runtime->start();

    // Insert into map
    RuntimeRecord record;
    record.runtime = std::move(runtime);

    auto [it, inserted] = runtimes_.emplace(runtime_id, std::move(record));
    if (!inserted) {
        throw GenAIException(
            GenAIErrorCode::INTERNAL_ERROR,
            "Runtime for model '" + runtime_id.model_id +
                "' replica " + std::to_string(runtime_id.replica_id) +
                "' already exists",
            500);
    }

    auto& bucket = replicas_by_model_[runtime_id.model_id];
    bucket.push_back(runtime_id);
    std::sort(bucket.begin(), bucket.end(),
              [](const qai_forge::execution::RuntimeId& lhs,
                 const qai_forge::execution::RuntimeId& rhs) {
                  return lhs.replica_id < rhs.replica_id;
              });

    LOG_DEBUG("[PredictiveModelPool] Runtime created for model '"
              << runtime_id.model_id << "' replica=" << runtime_id.replica_id);
    return it->second;
}

PredictiveModelPool::RuntimeRecord*
PredictiveModelPool::firstReplicaRecordLocked(const std::string& model_id) {
    const auto* replicas = replicasForModelLocked(model_id);
    if (!replicas || replicas->empty()) {
        return nullptr;
    }
    auto it = runtimes_.find(replicas->front());
    return it != runtimes_.end() ? &it->second : nullptr;
}

const std::vector<qai_forge::execution::RuntimeId>*
PredictiveModelPool::replicasForModelLocked(const std::string& model_id) const {
    auto it = replicas_by_model_.find(model_id);
    return it != replicas_by_model_.end() ? &it->second : nullptr;
}

size_t PredictiveModelPool::activeModelCount() const {
    size_t count = 0;
    for (const auto& [runtime_id, record] : runtimes_) {
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
    const auto config =
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
