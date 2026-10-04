// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/scheduler/WarmModelPool.h"

#include "qai_forge/scheduler/EvictionPolicy.h"
#include "qai_forge/backend/BackendFactory.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/managers/SystemResourceManager.h"
#include "qai_forge/utils/Logger.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace scheduler {

namespace {

ModelRuntimePairFactory defaultRuntimeFactory() {
    return [](const std::string& model_id) {
        return BackendFactory::createRuntimePair(model_id);
    };
}

SubmitResult rejected(SubmitStatus status,
                      const std::string& job_id,
                      std::string message) {
    SubmitResult result;
    result.status = status;
    result.job_id = job_id;
    result.message = std::move(message);
    return result;
}

std::chrono::milliseconds effectiveTtl(
    std::chrono::milliseconds ttl,
    std::chrono::milliseconds default_ttl) {
    return ttl.count() > 0 ? ttl : default_ttl;
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

const char* actionToString(int type) {
    switch (type) {
        case 0:
            return "activate";
        case 1:
            return "drain";
        case 2:
            return "reject";
    }
    return "unknown";
}

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

WarmModelPool::WarmModelPool(WarmModelPoolConfig config,
                             ModelRuntimePairFactory runtime_factory,
                             ModelRuntimeEvents runtime_events,
                             std::shared_ptr<ConversationMemoryCoordinator>
                                 memory_coordinator,
                             std::shared_ptr<ModelLoadCoordinator>
                                 load_coordinator,
                             std::shared_ptr<PlacementAdmissionGateway> placement,
                             std::shared_ptr<IRequestRoutingPolicy> routing_policy)
    : config_(config),
      runtime_factory_(runtime_factory ? std::move(runtime_factory)
                                       : defaultRuntimeFactory()),
      runtime_events_(std::move(runtime_events)),
      memory_coordinator_(std::move(memory_coordinator)),
      load_coordinator_(std::move(load_coordinator)),
      placement_(std::move(placement)),
      routing_policy_(routing_policy ? std::move(routing_policy)
                                     : defaultRoutingPolicy()),
      eviction_policy_(std::make_unique<EvictionPolicy>()) {
    if (!runtime_factory_) {
        throw std::invalid_argument("WarmModelPool requires a runtime factory");
    }
    if (!memory_coordinator_) {
        memory_coordinator_ =
            std::make_shared<ConversationMemoryCoordinator>();
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
    if (config_.cold_model_fairness_wait.count() <= 0) {
        config_.cold_model_fairness_wait = std::chrono::seconds(120);
    }
    if (config_.tool_response_timeout.count() <= 0) {
        config_.tool_response_timeout = std::chrono::seconds(30);
    }
    if (config_.max_replicas_per_model == 0) {
        config_.max_replicas_per_model = 1;
    }
    LOG_INFO("[WarmModelPool] Configured: max_active_models="
             << config_.max_active_models
             << " idle_timeout_ms=" << config_.idle_timeout.count()
             << " cold_model_fairness_wait_ms="
             << config_.cold_model_fairness_wait.count()
             << " tool_response_timeout_ms="
             << config_.tool_response_timeout.count()
             << " memory_headroom_mb=" << config_.memory_headroom_mb
             << " max_replicas_per_model=" << config_.max_replicas_per_model);
}

WarmModelPool::~WarmModelPool() {
    stop(false);
}

void WarmModelPool::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
        return;
    }

    shutdown_requested_ = false;
    event_pending_ = true;
    started_ = true;
    event_thread_ = std::thread(&WarmModelPool::eventLoop, this);
    cv_.notify_one();
    LOG_INFO("[WarmModelPool] Started event loop");
}

ModelRuntime* WarmModelPool::reserve(const std::string& model_id,
                                     const std::string& session_id) {
    if (model_id.empty()) {
        throw GenAIException(
            GenAIErrorCode::INVALID_REQUEST,
            "Cannot reserve a generative runtime without model_id",
            400);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_requested_) {
        throw GenAIException(
            GenAIErrorCode::HARDWARE_UNAVAILABLE,
            "WarmModelPool is shutting down",
            503);
    }

    const auto now = std::chrono::steady_clock::now();

    // Session affinity takes priority over routing_policy_: a session's
    // 2nd+ turn must land on the same replica as its 1st turn so stateful
    // backends' per-session KV-cache stays valid. See SessionAffinity's
    // doc comment in WarmModelPool.h.
    RuntimeRecord* affine = nullptr;
    if (!session_id.empty()) {
        affine = sessionAffinityTargetLocked(model_id, session_id);
    }

    RuntimeRecord& record = affine
        ? *affine
        : routeOrCreateRuntimeLocked(model_id, now);

    if (!session_id.empty()) {
        session_affinity_[sessionAffinityKey(model_id, session_id)] =
            SessionAffinity{record.runtime->runtimeId(), now};
    }

    ++record.reservation_count;
    record.last_used_at = now;
    event_pending_ = true;
    cv_.notify_one();
    LOG_DEBUG("[WarmModelPool] Reserved runtime: model=" << model_id
              << " replica=" << record.runtime->runtimeId().replica_id
              << " session_affinity=" << (affine ? "hit" : "miss")
              << " pending_reservations=" << record.reservation_count);
    return record.runtime.get();
}

SubmitResult WarmModelPool::submit(ModelRuntime* runtime,
                                   GenerativeJobPtr job) {
    if (!job) {
        return rejected(
            SubmitStatus::REJECTED_MODEL_NOT_FOUND,
            {},
            "WarmModelPool received null generative job");
    }

    if (!runtime || job->model_id.empty()) {
        return rejected(
            SubmitStatus::REJECTED_MODEL_NOT_FOUND,
            job->job_id,
            "Generative job is missing its reserved runtime or model_id");
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_requested_) {
            return rejected(
                SubmitStatus::REJECTED_SHUTTING_DOWN,
                job->job_id,
                "WarmModelPool is shutting down");
        }

        auto it = runtimes_.find(runtime->runtimeId());
        if (it == runtimes_.end() || it->second.runtime.get() != runtime ||
            it->second.runtime->modelId() != job->model_id) {
            return rejected(
                SubmitStatus::REJECTED_MODEL_NOT_FOUND,
                job->job_id,
                "Reserved runtime does not match model '" + job->model_id + "'");
        }

        const auto now = std::chrono::steady_clock::now();
        RuntimeRecord& record = it->second;

        if (config_.max_queue_depth_per_model > 0) {
            const ModelRuntimeSnapshot runtime_snapshot = record.runtime->snapshot();
            if (runtime_snapshot.queue.total() >=
                config_.max_queue_depth_per_model) {
                return rejected(
                    SubmitStatus::REJECTED_QUEUE_FULL,
                    job->job_id,
                    "Model queue is full for model '" + job->model_id + "'");
            }
        }

        record.last_used_at = now;
        if (config_.queue_admission_timeout.count() > 0) {
            job->enqueue_deadline = now + config_.queue_admission_timeout;
        }
        record.runtime->enqueue(job);
        event_pending_ = true;
        LOG_INFO("[WarmModelPool] Enqueued job: job=" << job->job_id
                 << " model=" << job->model_id
                 << " replica=" << runtime->runtimeId().replica_id
                 << " runtime_state=" << stateToString(record.state)
                 << " active_reserved="
                 << (record.active_reserved ? "true" : "false"));
    }

    cv_.notify_one();
    return SubmitResult{
        SubmitStatus::QUEUED,
        job->job_id,
        "queued",
    };
}

void WarmModelPool::releaseReservation(ModelRuntime* runtime) noexcept {
    if (!runtime) {
        return;
    }

    bool released = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : runtimes_) {
            RuntimeRecord& record = entry.second;
            if (record.runtime.get() != runtime || record.reservation_count == 0) {
                continue;
            }
            --record.reservation_count;
            event_pending_ = true;
            released = true;
            LOG_DEBUG("[WarmModelPool] Released runtime reservation: model="
                      << entry.first.model_id
                      << " replica=" << entry.first.replica_id
                      << " pending_reservations="
                      << record.reservation_count);
            break;
        }
    }
    if (released) {
        cv_.notify_one();
    }
}

CancelResult WarmModelPool::cancel(const std::string& job_id) {
    if (job_id.empty()) {
        return CancelResult{
            CancelStatus::NOT_FOUND,
            job_id,
            "Cancel request is missing job_id"};
    }

    std::vector<ModelRuntime*> runtimes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        runtimes.reserve(runtimes_.size());
        for (auto& entry : runtimes_) {
            runtimes.push_back(entry.second.runtime.get());
        }
    }

    for (ModelRuntime* runtime : runtimes) {
        if (!runtime) {
            continue;
        }

        CancelResult result = runtime->cancel(job_id);
        if (result.cancelled()) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                event_pending_ = true;
            }
            cv_.notify_one();
            return result;
        }
    }

    return CancelResult{
        CancelStatus::NOT_FOUND,
        job_id,
        "No queued or running scheduler job found for job_id '" + job_id + "'"};
}

bool WarmModelPool::openToolLease(const std::string& model_id,
                                  const std::string& chain_id,
                                  std::chrono::milliseconds ttl,
                                  const std::string& session_id) {
    if (model_id.empty() || chain_id.empty()) {
        return false;
    }

    std::vector<ModelRuntime*> runtimes_to_reactivate;
    bool any_found = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::steady_clock::now();

        // Affinity-targeted path: operate on exactly one replica.
        RuntimeRecord* affine = session_id.empty()
            ? nullptr
            : sessionAffinityTargetLocked(model_id, session_id);
        if (affine) {
            RuntimeRecord& record = *affine;
            const qai_forge::execution::RuntimeId runtime_id =
                record.runtime->runtimeId();
            any_found = true;
            record.tool_lease.chain_id = chain_id;
            record.tool_lease.lease_until =
                now + effectiveTtl(ttl, config_.tool_response_timeout);
            record.eviction_requested = false;
            LOG_INFO("[WarmModelPool] Opened tool lease (affinity): model="
                     << model_id
                     << " replica=" << runtime_id.replica_id
                     << " chain=" << chain_id
                     << " ttl_ms="
                     << effectiveTtl(ttl, config_.tool_response_timeout).count());
            if (record.state == ModelRuntimeState::Running ||
                record.state == ModelRuntimeState::Draining) {
                runtimes_to_reactivate.push_back(record.runtime.get());
            }
            event_pending_ = true;
        } else {
            // Broadcast fallback — no affinity known (empty session_id or
            // stale/missing mapping).
            const auto* replicas = replicasForModelLocked(model_id);
            if (!replicas || replicas->empty()) {
                return false;
            }
            for (const auto& runtime_id : *replicas) {
                auto it = runtimes_.find(runtime_id);
                if (it == runtimes_.end()) {
                    continue;
                }
                any_found = true;
                RuntimeRecord& record = it->second;
                record.tool_lease.chain_id = chain_id;
                record.tool_lease.lease_until =
                    now + effectiveTtl(ttl, config_.tool_response_timeout);
                record.eviction_requested = false;
                LOG_INFO("[WarmModelPool] Opened tool lease (broadcast): model="
                         << model_id
                         << " replica=" << runtime_id.replica_id
                         << " chain=" << chain_id
                         << " ttl_ms="
                         << effectiveTtl(ttl, config_.tool_response_timeout).count());
                if (record.state == ModelRuntimeState::Running ||
                    record.state == ModelRuntimeState::Draining) {
                    runtimes_to_reactivate.push_back(record.runtime.get());
                }
            }
            if (any_found) {
                event_pending_ = true;
            }
        }
    }

    for (ModelRuntime* runtime : runtimes_to_reactivate) {
        runtime->activate();
    }

    if (any_found) {
        cv_.notify_one();
    }
    return any_found;
}

bool WarmModelPool::renewToolLease(const std::string& model_id,
                                   const std::string& chain_id,
                                   std::chrono::milliseconds ttl,
                                   const std::string& session_id) {
    return openToolLease(model_id, chain_id, ttl, session_id);
}

bool WarmModelPool::closeToolLease(const std::string& model_id,
                                   const std::string& chain_id,
                                   const std::string& session_id) {
    if (model_id.empty() || chain_id.empty()) {
        return false;
    }

    bool any_closed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        RuntimeRecord* affine = session_id.empty()
            ? nullptr
            : sessionAffinityTargetLocked(model_id, session_id);
        if (affine) {
            RuntimeRecord& record = *affine;
            if (record.tool_lease.chain_id == chain_id) {
                const qai_forge::execution::RuntimeId runtime_id =
                    record.runtime->runtimeId();
                clearToolLeaseLocked(record);
                any_closed = true;
                LOG_INFO("[WarmModelPool] Closed tool lease (affinity): model="
                         << model_id
                         << " replica=" << runtime_id.replica_id
                         << " chain=" << chain_id);
            }
        } else {
            const auto* replicas = replicasForModelLocked(model_id);
            if (!replicas) {
                return false;
            }

            for (const auto& runtime_id : *replicas) {
                auto it = runtimes_.find(runtime_id);
                if (it == runtimes_.end()) {
                    continue;
                }
                RuntimeRecord& record = it->second;
                if (record.tool_lease.chain_id != chain_id) {
                    continue;
                }
                clearToolLeaseLocked(record);
                any_closed = true;
                LOG_INFO("[WarmModelPool] Closed tool lease (broadcast): model="
                         << model_id
                         << " replica=" << runtime_id.replica_id
                         << " chain=" << chain_id);
            }
        }
        if (any_closed) {
            event_pending_ = true;
        }
    }

    if (any_closed) {
        cv_.notify_one();
    }
    return any_closed;
}

void WarmModelPool::checkpoint() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_requested_) {
            return;
        }

        event_pending_ = true;
    }

    cv_.notify_one();
}

ModelPoolSnapshot WarmModelPool::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshotLocked();
}

void WarmModelPool::clearSession(const std::string& model_id,
                                  const std::string& session_id) {
    if (model_id.empty()) return;

    std::vector<ModelRuntime*> targets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        RuntimeRecord* affine = session_id.empty()
            ? nullptr
            : sessionAffinityTargetLocked(model_id, session_id);
        if (affine) {
            targets.push_back(affine->runtime.get());
        } else {
            // Broadcast fallback — see class-level comment on
            // clearSession() in WarmModelPool.h for rationale.
            const auto* replicas = replicasForModelLocked(model_id);
            if (!replicas) return;
            for (const auto& runtime_id : *replicas) {
                auto it = runtimes_.find(runtime_id);
                if (it != runtimes_.end() && it->second.runtime) {
                    targets.push_back(it->second.runtime.get());
                }
            }
        }
    }
    for (ModelRuntime* runtime : targets) {
        runtime->clearSession(session_id);
    }
}

ModelPoolSnapshot WarmModelPool::snapshotLocked() const {

    ModelPoolSnapshot snapshot;
    snapshot.max_active_models = config_.max_active_models;
    snapshot.active_reserved_models = activeReservedCountLocked();
    snapshot.runtime_count = runtimes_.size();
    snapshot.runtimes.reserve(runtimes_.size());

    for (const auto& entry : runtimes_) {
        const RuntimeRecord& record = entry.second;
        const ModelRuntimeAdmissionSnapshot runtime_snapshot =
            record.runtime->admissionSnapshot();
        snapshot.runtimes.push_back(ModelPoolRuntimeSnapshot{
            entry.first.model_id,
            entry.first,
            runtime_snapshot.state,
            runtime_snapshot.queue,
            runtime_snapshot.candidate,
            runtime_snapshot.has_running_job,
            runtime_snapshot.running_job_id,
            runtime_snapshot.healthy,
            record.active_reserved,
            record.reservation_count,
            record.eviction_requested,
            record.tool_lease.active(),
            record.tool_lease.chain_id,
            record.tool_lease.lease_until,
            record.last_used_at,
            record.idle_since,
        });
    }

    return snapshot;
}

void WarmModelPool::refreshTimedStateLocked(
    std::chrono::steady_clock::time_point now) {
    for (auto& entry : runtimes_) {
        RuntimeRecord& record = entry.second;

        if (record.tool_lease.lease_until &&
            now >= *record.tool_lease.lease_until) {
            LOG_WARN("[WarmModelPool] Tool lease expired: model="
                     << entry.first.model_id
                     << " replica=" << entry.first.replica_id
                     << " chain=" << record.tool_lease.chain_id);
            clearToolLeaseLocked(record);
        }

        if (config_.queue_admission_timeout.count() > 0 && record.runtime) {
            record.runtime->expireStaleQueuedJobs();
        }
    }

    pruneSessionAffinityLocked(now);
}

void WarmModelPool::stop(bool force) {
    std::vector<ModelRuntime*> runtimes;
    bool should_join = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_requested_ = true;
        event_pending_ = true;

        runtimes.reserve(runtimes_.size());
        for (auto& entry : runtimes_) {
            runtimes.push_back(entry.second.runtime.get());
        }

        should_join = event_thread_.joinable() &&
                      event_thread_.get_id() != std::this_thread::get_id();
    }

    if (owns_load_coordinator_) {
        load_coordinator_->shutdown();
    }
    cv_.notify_one();

    if (should_join) {
        event_thread_.join();
    }

    for (ModelRuntime* runtime : runtimes) {
        if (runtime) {
            runtime->stop(force);
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        started_ = false;
    }
    LOG_INFO("[WarmModelPool] Stopped force=" << (force ? "true" : "false"));
}

void WarmModelPool::eventLoop() {
    std::unique_lock<std::mutex> lock(mutex_);

    while (!shutdown_requested_) {
        const auto now = std::chrono::steady_clock::now();
        std::vector<PoolAction> actions = planActionsLocked(now);

        if (!actions.empty()) {
            lock.unlock();
            applyActions(std::move(actions));
            lock.lock();
            continue;
        }

        const auto next_deadline = nextPolicyDeadlineLocked(now);
        if (next_deadline == std::chrono::steady_clock::time_point::max()) {
            cv_.wait(lock, [this]() {
                return shutdown_requested_ || event_pending_;
            });
        } else {
            cv_.wait_until(lock, next_deadline, [this]() {
                return shutdown_requested_ || event_pending_;
            });
        }

        event_pending_ = false;
    }
}

std::vector<WarmModelPool::PoolAction> WarmModelPool::planActionsLocked(
    std::chrono::steady_clock::time_point now) {
    std::vector<PoolAction> actions;

    refreshTimedStateLocked(now);

    EvictionPolicyInput input;
    input.snapshot = snapshotLocked();
    input.config = EvictionPolicyConfig{
        config_.max_active_models,
        config_.idle_timeout,
        config_.cold_model_fairness_wait,
        config_.memory_headroom_mb,
    };
    input.available_memory_mb = availableMemoryMb();
    input.now = now;
    input.model_memory_mb.reserve(input.snapshot.runtimes.size());
    for (const auto& runtime : input.snapshot.runtimes) {
        input.model_memory_mb.emplace(
            runtime.model_id,
            modelMemoryMb(runtime.model_id));
    }

    const EvictionPolicyPlan policy_plan = eviction_policy_->plan(input);
    for (const EvictionPolicyAction& action : policy_plan.actions) {
        // EvictionPolicy now reasons per-replica (full RuntimeId) — look
        // the record up directly instead of via the old
        // "one-representative-per-model" shortcut.
        auto it = runtimes_.find(action.runtime_id);
        if (it == runtimes_.end()) {
            continue;
        }
        RuntimeRecord* record = &it->second;
        const qai_forge::execution::RuntimeId runtime_id = action.runtime_id;

        PoolActionType pool_type = PoolActionType::Activate;
        ModelLoadCoordinator::LoadReservation load_reservation;
        switch (action.type) {
            case EvictionPolicyActionType::Activate:
                load_reservation = load_coordinator_->tryAcquire(
                    runtime_id.model_id,
                    modelMemoryMb(runtime_id.model_id));
                if (!load_reservation) {
                    LOG_INFO("[WarmModelPool] Shared load admission changed; keeping model queued: model="
                             << runtime_id.model_id);
                    continue;
                }
                record->active_reserved = true;
                record->eviction_requested = false;
                pool_type = PoolActionType::Activate;
                break;
            case EvictionPolicyActionType::Drain:
                record->eviction_requested = true;
                pool_type = PoolActionType::Drain;
                break;
            case EvictionPolicyActionType::Reject:
                pool_type = PoolActionType::Reject;
                break;
        }

        actions.push_back(PoolAction{
            pool_type,
            runtime_id,
            record->runtime.get(),
            std::move(load_reservation),
        });
        LOG_INFO("[WarmModelPool] Planned action: action="
                 << actionToString(static_cast<int>(pool_type))
                 << " model=" << runtime_id.model_id
                 << " replica=" << runtime_id.replica_id
                 << " available_mb=" << input.available_memory_mb
                 << " active_reserved="
                 << input.snapshot.active_reserved_models
                 << "/" << input.snapshot.max_active_models);
    }

    return actions;
}

void WarmModelPool::applyActions(std::vector<PoolAction> actions) {
    for (PoolAction& action : actions) {
        if (!action.runtime) {
            continue;
        }

        if (action.type == PoolActionType::Activate) {
            LOG_INFO("[WarmModelPool] Applying action: activate model="
                     << action.runtime_id.model_id);
            if (!action.runtime->activate(
                    std::move(action.load_reservation))) {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = runtimes_.find(action.runtime_id);
                if (it != runtimes_.end()) {
                    it->second.active_reserved = false;
                    it->second.eviction_requested = false;
                }
                event_pending_ = true;
                cv_.notify_one();
            }
            continue;
        }

        if (action.type == PoolActionType::Reject) {
            LOG_WARN("[WarmModelPool] Applying action: reject model="
                     << action.runtime_id.model_id);
            action.runtime->failQueued(GenAIException(
                GenAIErrorCode::INSUFFICIENT_MEMORY,
                "Model '" + action.runtime_id.model_id +
                    "' cannot be admitted with current memory constraints",
                503));
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = runtimes_.find(action.runtime_id);
                if (it != runtimes_.end()) {
                    it->second.active_reserved = false;
                    it->second.eviction_requested = false;
                }
                event_pending_ = true;
            }
            cv_.notify_one();
            continue;
        }

        LOG_INFO("[WarmModelPool] Applying action: drain model="
                 << action.runtime_id.model_id);
        action.runtime->requestDrain();
    }
}

void WarmModelPool::handleRuntimeStateChanged(
    const qai_forge::execution::RuntimeId& runtime_id,
    ModelRuntimeState state) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = runtimes_.find(runtime_id);
        if (it == runtimes_.end()) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        RuntimeRecord& record = it->second;
        const ModelRuntimeState old_state = record.state;
        record.state = state;

        if (isActiveReservedState(state)) {
            record.active_reserved = true;
        }

        switch (state) {
            case ModelRuntimeState::Loading:
                record.idle_since.reset();
                record.eviction_requested = false;
                break;
            case ModelRuntimeState::Running:
                record.idle_since.reset();
                record.last_used_at = now;
                break;
            case ModelRuntimeState::Idle:
                record.idle_since = now;
                record.last_used_at = now;
                record.eviction_requested = false;
                break;
            case ModelRuntimeState::Draining:
            case ModelRuntimeState::Evicting:
                record.idle_since.reset();
                record.eviction_requested = true;
                break;
            case ModelRuntimeState::NotResident:
            case ModelRuntimeState::Failed:
            case ModelRuntimeState::Stopped:
                record.active_reserved = false;
                record.idle_since.reset();
                record.eviction_requested = false;
                clearToolLeaseLocked(record);
                break;
        }

        event_pending_ = true;
        LOG_INFO("[WarmModelPool] Runtime state changed: model="
                 << runtime_id.model_id
                 << " replica=" << runtime_id.replica_id
                 << " " << stateToString(old_state)
                 << " -> " << stateToString(state)
                 << " active_reserved="
                 << (record.active_reserved ? "true" : "false")
                 << " eviction_requested="
                 << (record.eviction_requested ? "true" : "false")
                 << " tool_lease="
                 << (record.tool_lease.active() ? "true" : "false"));
    }

    cv_.notify_one();
}

WarmModelPool::RuntimeRecord& WarmModelPool::routeOrCreateRuntimeLocked(
    const std::string& model_id,
    std::chrono::steady_clock::time_point now) {
    auto replicas_it = replicas_by_model_.find(model_id);
    if (replicas_it == replicas_by_model_.end() ||
        replicas_it->second.empty()) {
        // No resident replica yet — create replica_id=0 unconditionally.
        return getOrCreateRuntimeLocked(
            qai_forge::execution::RuntimeId{model_id, 0}, now);
    }

    std::vector<RuntimeCandidate> candidates;
    candidates.reserve(replicas_it->second.size());
    for (const auto& runtime_id : replicas_it->second) {
        auto it = runtimes_.find(runtime_id);
        if (it == runtimes_.end()) {
            continue;
        }
        const RuntimeRecord& record = it->second;
        const ModelRuntimeAdmissionSnapshot admission =
            record.runtime->admissionSnapshot();
        candidates.push_back(RuntimeCandidate{
            runtime_id,
            toRoutingState(record.state),
            admission.queue.total(),
            admission.has_running_job,
            admission.healthy,
        });
    }

    const auto chosen = routing_policy_->choose(candidates);
    if (chosen) {
        auto it = runtimes_.find(*chosen);
        if (it != runtimes_.end()) {
            return it->second;
        }
    }

    // No candidate can currently accept dispatch. Grow a new replica if
    // under the configured cap; otherwise fall back to the first
    // (lowest replica_id) existing replica so the request still queues
    // rather than being silently dropped — identical to single-replica
    // behavior when max_replicas_per_model == 1 (growth never triggers).
    if (replicas_it->second.size() < config_.max_replicas_per_model) {
        const uint64_t next_replica_id =
            replicas_it->second.back().replica_id + 1;
        return getOrCreateRuntimeLocked(
            qai_forge::execution::RuntimeId{model_id, next_replica_id}, now);
    }

    RuntimeRecord* fallback = firstReplicaRecordLocked(model_id);
    if (fallback) {
        return *fallback;
    }

    // Unreachable in practice (replicas_by_model_ was non-empty above),
    // but keep the function total: fall through to replica_id=0 creation.
    return getOrCreateRuntimeLocked(
        qai_forge::execution::RuntimeId{model_id, 0}, now);
}

WarmModelPool::RuntimeRecord& WarmModelPool::getOrCreateRuntimeLocked(
    const qai_forge::execution::RuntimeId& runtime_id,
    std::chrono::steady_clock::time_point now) {
    auto existing = runtimes_.find(runtime_id);
    if (existing != runtimes_.end()) {
        return existing->second;
    }

    // ModelRuntimeEvents::on_state_changed is defined in terms of model_id
    // alone (shared with PredictiveModelPool, which has no replica
    // concept). Capture this replica's full RuntimeId by value so the
    // callback can report state changes against the correct replica
    // rather than just the bare model_id string ModelRuntime passes back.
    ModelRuntimeEvents events = runtime_events_;
    const qai_forge::execution::RuntimeId captured_runtime_id = runtime_id;
    events.on_state_changed =
        [this, captured_runtime_id](const std::string& /*model_id*/,
                                    ModelRuntimeState state) {
            handleRuntimeStateChanged(captured_runtime_id, state);
        };

    // Create backend + orchestrator pair via factory and inject the
    // memory coordinator into the orchestrator adapter — conversation
    // memory bookkeeping lives inside OrchestratorAdapter.
    RuntimePair pair = runtime_factory_(runtime_id.model_id);
    pair.orchestrator->attachMemoryCoordinator(memory_coordinator_);

    auto runtime = std::make_unique<ModelRuntime>(
        runtime_id.model_id,
        std::move(pair.backend),
        std::move(pair.orchestrator),
        load_coordinator_,
        modelMemoryMb(runtime_id.model_id),
        placement_,
        std::move(events),
        runtime_id.replica_id);
    runtime->start();

    RuntimeRecord record;
    record.runtime = std::move(runtime);
    record.state = ModelRuntimeState::NotResident;
    record.active_reserved = false;
    record.reservation_count = 0;
    record.eviction_requested = false;
    record.last_used_at = now;

    auto inserted = runtimes_.emplace(runtime_id, std::move(record));
    auto& bucket = replicas_by_model_[runtime_id.model_id];
    bucket.push_back(runtime_id);
    std::sort(bucket.begin(), bucket.end(),
              [](const qai_forge::execution::RuntimeId& lhs,
                 const qai_forge::execution::RuntimeId& rhs) {
                  return lhs.replica_id < rhs.replica_id;
              });
    LOG_INFO("[WarmModelPool] Created runtime: model=" << runtime_id.model_id
             << " replica=" << runtime_id.replica_id);
    return inserted.first->second;
}

WarmModelPool::RuntimeRecord* WarmModelPool::firstReplicaRecordLocked(
    const std::string& model_id) {
    const auto* replicas = replicasForModelLocked(model_id);
    if (!replicas || replicas->empty()) {
        return nullptr;
    }
    auto it = runtimes_.find(replicas->front());
    return it != runtimes_.end() ? &it->second : nullptr;
}

const std::vector<qai_forge::execution::RuntimeId>*
WarmModelPool::replicasForModelLocked(const std::string& model_id) const {
    auto it = replicas_by_model_.find(model_id);
    return it != replicas_by_model_.end() ? &it->second : nullptr;
}

WarmModelPool::RuntimeRecord* WarmModelPool::sessionAffinityTargetLocked(
    const std::string& model_id,
    const std::string& session_id) {
    auto it = session_affinity_.find(sessionAffinityKey(model_id, session_id));
    if (it == session_affinity_.end()) {
        return nullptr;
    }

    auto runtime_it = runtimes_.find(it->second.runtime_id);
    if (runtime_it == runtimes_.end() ||
        isTerminalState(runtime_it->second.state)) {
        // Stale mapping — target replica is gone or dead. The caller's
        // next request will cold-start on whatever replica routing_policy_
        // picks; ConversationMemoryCoordinator reconstructs context on
        // reload (pre-existing single-replica eviction-recovery path).
        session_affinity_.erase(it);
        return nullptr;
    }

    return &runtime_it->second;
}

std::string WarmModelPool::sessionAffinityKey(const std::string& model_id,
                                              const std::string& session_id) {
    std::string key;
    key.reserve(model_id.size() + session_id.size() + 1);
    key += model_id;
    key += '\x1f';
    key += session_id;
    return key;
}

void WarmModelPool::pruneSessionAffinityLocked(
    std::chrono::steady_clock::time_point now) {
    if (config_.idle_timeout.count() <= 0) {
        return;
    }
    for (auto it = session_affinity_.begin(); it != session_affinity_.end();) {
        if (now - it->second.last_used >= config_.idle_timeout) {
            it = session_affinity_.erase(it);
        } else {
            ++it;
        }
    }
}

size_t WarmModelPool::activeReservedCountLocked() const {
    size_t count = 0;
    for (const auto& entry : runtimes_) {
        if (entry.second.active_reserved) {
            ++count;
        }
    }
    return count;
}

long WarmModelPool::availableMemoryMb() const {
    return load_coordinator_->availableMemoryMb();
}

long WarmModelPool::modelMemoryMb(const std::string& model_id) const {
    const int memory_mb =
        ModelConfigManager::getInstance().getMemoryRequirementMb(model_id);
    return memory_mb > 0 ? memory_mb : 4096;
}

bool WarmModelPool::runtimeHasQueuedWorkLocked(const RuntimeRecord& record) const {
    return record.runtime->admissionSnapshot().queue.total() > 0;
}

void WarmModelPool::clearToolLeaseLocked(RuntimeRecord& record) {
    record.tool_lease.clear();
}

std::chrono::steady_clock::time_point
WarmModelPool::nextPolicyDeadlineLocked(
    std::chrono::steady_clock::time_point now) const {
    auto next_deadline = std::chrono::steady_clock::time_point::max();
    const size_t active_reserved_count = activeReservedCountLocked();
    const long available_memory_mb = availableMemoryMb();

    for (const auto& entry : runtimes_) {
        const RuntimeRecord& record = entry.second;
        if (record.tool_lease.lease_until) {
            next_deadline = std::min(next_deadline, *record.tool_lease.lease_until);
        }

        if (config_.queue_admission_timeout.count() > 0 && record.runtime) {
            next_deadline = std::min(
                next_deadline,
                record.runtime->earliestQueueDeadline());
        }

        if (config_.idle_timeout.count() > 0 &&
            record.state == ModelRuntimeState::Idle &&
            record.active_reserved &&
            record.reservation_count == 0 &&
            !record.eviction_requested &&
            !record.tool_lease.active() &&
            record.idle_since &&
            !runtimeHasQueuedWorkLocked(record)) {
            next_deadline = std::min(
                next_deadline,
                *record.idle_since + config_.idle_timeout);
        }

        const bool is_cold_waiting =
            (record.state == ModelRuntimeState::NotResident ||
             record.state == ModelRuntimeState::Failed) &&
            !record.active_reserved;
        if (!is_cold_waiting) {
            continue;
        }

        const ModelRuntimeAdmissionSnapshot admission =
            record.runtime->admissionSnapshot();
        if (!admission.candidate.has_work) {
            continue;
        }

        const long required_memory_mb =
            modelMemoryMb(entry.first.model_id) + config_.memory_headroom_mb;
        const bool blocked_by_capacity =
            active_reserved_count + 1 > config_.max_active_models;
        const bool blocked_by_memory =
            available_memory_mb < required_memory_mb;
        if (!blocked_by_capacity && !blocked_by_memory) {
            continue;
        }

        const auto fairness_deadline =
            admission.candidate.created_at + config_.cold_model_fairness_wait;
        if (fairness_deadline > now) {
            next_deadline = std::min(next_deadline, fairness_deadline);
        }
    }

    // Session-affinity entries also need a wakeup at their prune deadline
    // so stale mappings don't linger past idle_timeout indefinitely when
    // no other activity is driving the event loop.
    if (config_.idle_timeout.count() > 0) {
        for (const auto& entry : session_affinity_) {
            next_deadline = std::min(
                next_deadline,
                entry.second.last_used + config_.idle_timeout);
        }
    }

    return next_deadline;
}

bool WarmModelPool::isActiveReservedState(ModelRuntimeState state) {
    return state == ModelRuntimeState::Loading ||
           state == ModelRuntimeState::Idle ||
           state == ModelRuntimeState::Running ||
           state == ModelRuntimeState::Draining ||
           state == ModelRuntimeState::Evicting;
}

bool WarmModelPool::isTerminalState(ModelRuntimeState state) {
    return state == ModelRuntimeState::NotResident ||
           state == ModelRuntimeState::Failed ||
           state == ModelRuntimeState::Stopped;
}

} // namespace scheduler
