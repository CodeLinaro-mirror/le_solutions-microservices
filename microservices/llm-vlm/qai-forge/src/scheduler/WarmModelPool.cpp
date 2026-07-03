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
            return "protected_drain";
        case 3:
            return "reject";
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

} // namespace

WarmModelPool::WarmModelPool(WarmModelPoolConfig config,
                             ModelRuntimePairFactory runtime_factory)
    : config_(config),
      runtime_factory_(runtime_factory ? std::move(runtime_factory)
                                       : defaultRuntimeFactory()),
      eviction_policy_(std::make_unique<EvictionPolicy>()) {
    if (!runtime_factory_) {
        throw std::invalid_argument("WarmModelPool requires a runtime factory");
    }
    if (config_.max_concurrent_model_loads == 0) {
        config_.max_concurrent_model_loads = 1;
    }
    if (config_.model_residency_ttl.count() <= 0) {
        config_.model_residency_ttl = std::chrono::seconds(60);
    }
    if (config_.tool_response_timeout.count() <= 0) {
        config_.tool_response_timeout = std::chrono::seconds(30);
    }
    LOG_INFO("[WarmModelPool] Configured: max_active_models="
             << config_.max_active_models
             << " max_concurrent_model_loads="
             << config_.max_concurrent_model_loads
             << " idle_timeout_ms=" << config_.idle_timeout.count()
             << " residency_ttl_ms=" << config_.model_residency_ttl.count()
             << " tool_response_timeout_ms="
             << config_.tool_response_timeout.count()
             << " memory_headroom_mb=" << config_.memory_headroom_mb
             << " running_cancel_mode="
             << runningCancelModeToString(config_.running_cancel_mode));
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

SubmitResult WarmModelPool::submit(InferenceJobPtr job) {
    if (!job) {
        return rejected(
            SubmitStatus::REJECTED_MODEL_NOT_FOUND,
            {},
            "WarmModelPool received null inference job");
    }

    if (job->model_id.empty()) {
        return rejected(
            SubmitStatus::REJECTED_MODEL_NOT_FOUND,
            job->job_id,
            "Inference job is missing model_id");
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_requested_) {
            return rejected(
                SubmitStatus::REJECTED_SHUTTING_DOWN,
                job->job_id,
                "WarmModelPool is shutting down");
        }

        const auto now = std::chrono::steady_clock::now();
        RuntimeRecord& record = getOrCreateRuntimeLocked(job->model_id, now);

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
        record.runtime->enqueue(job);
        event_pending_ = true;
        LOG_INFO("[WarmModelPool] Enqueued job: job=" << job->job_id
                 << " model=" << job->model_id
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
                                  std::chrono::milliseconds ttl) {
    if (model_id.empty() || chain_id.empty()) {
        return false;
    }

    ModelRuntime* runtime_to_reactivate = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = runtimes_.find(model_id);
        if (it == runtimes_.end()) {
            return false;
        }

        const auto now = std::chrono::steady_clock::now();
        RuntimeRecord& record = it->second;
        record.tool_chain_id = chain_id;
        record.tool_lease_until =
            now + effectiveTtl(ttl, config_.tool_response_timeout);
        record.eviction_requested = false;
        LOG_INFO("[WarmModelPool] Opened tool lease: model=" << model_id
                 << " chain=" << chain_id
                 << " ttl_ms="
                 << effectiveTtl(ttl, config_.tool_response_timeout).count());
        if (record.state == ModelRuntimeState::Running ||
            record.state == ModelRuntimeState::Draining ||
            record.state == ModelRuntimeState::Evicting) {
            runtime_to_reactivate = record.runtime.get();
        }
        event_pending_ = true;
    }

    if (runtime_to_reactivate) {
        runtime_to_reactivate->activate();
    }

    cv_.notify_one();
    return true;
}

bool WarmModelPool::renewToolLease(const std::string& model_id,
                                   const std::string& chain_id,
                                   std::chrono::milliseconds ttl) {
    return openToolLease(model_id, chain_id, ttl);
}

bool WarmModelPool::closeToolLease(const std::string& model_id,
                                   const std::string& chain_id) {
    if (model_id.empty() || chain_id.empty()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = runtimes_.find(model_id);
        if (it == runtimes_.end()) {
            return false;
        }

        RuntimeRecord& record = it->second;
        if (record.tool_chain_id != chain_id) {
            return false;
        }

        clearToolLeaseLocked(record);
        event_pending_ = true;
        LOG_INFO("[WarmModelPool] Closed tool lease: model=" << model_id
                 << " chain=" << chain_id);
    }

    cv_.notify_one();
    return true;
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
            entry.first,
            runtime_snapshot.state,
            runtime_snapshot.queue,
            runtime_snapshot.candidate,
            runtime_snapshot.has_running_job,
            runtime_snapshot.running_job_id,
            runtime_snapshot.healthy,
            record.active_reserved,
            record.eviction_requested,
            record.expiry_pending,
            static_cast<bool>(record.tool_lease_until),
            record.tool_chain_id,
            record.tool_lease_until,
            record.resident_since,
            record.residency_expires_at,
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

        if (record.tool_lease_until &&
            now >= *record.tool_lease_until) {
            LOG_WARN("[WarmModelPool] Tool lease expired: model="
                     << entry.first << " chain=" << record.tool_chain_id);
            clearToolLeaseLocked(record);
        }

        if (record.active_reserved &&
            isActiveReservedState(record.state) &&
            record.residency_expires_at &&
            !record.expiry_pending &&
            now >= *record.residency_expires_at) {
            record.expiry_pending = true;
            LOG_INFO("[WarmModelPool] Residency TTL expired: model="
                     << entry.first << " state=" << stateToString(record.state));
        }
    }
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
        std::vector<PoolAction> actions =
            planActionsLocked(std::chrono::steady_clock::now());

        if (!actions.empty()) {
            lock.unlock();
            applyActions(actions);
            lock.lock();
            continue;
        }

        const auto next_deadline = nextIdleDeadlineLocked();
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
        config_.max_concurrent_model_loads,
        config_.idle_timeout,
        config_.model_residency_ttl,
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
        auto it = runtimes_.find(action.model_id);
        if (it == runtimes_.end()) {
            continue;
        }

        PoolActionType pool_type = PoolActionType::Activate;
        switch (action.type) {
            case EvictionPolicyActionType::Activate:
                it->second.active_reserved = true;
                it->second.eviction_requested = false;
                pool_type = PoolActionType::Activate;
                break;
            case EvictionPolicyActionType::Drain:
                it->second.eviction_requested = true;
                pool_type = PoolActionType::Drain;
                break;
            case EvictionPolicyActionType::ProtectedDrain:
                it->second.eviction_requested = true;
                it->second.expiry_pending = true;
                pool_type = PoolActionType::ProtectedDrain;
                break;
            case EvictionPolicyActionType::Reject:
                pool_type = PoolActionType::Reject;
                break;
        }

        actions.push_back(PoolAction{
            pool_type,
            action.model_id,
            it->second.runtime.get(),
        });
        LOG_INFO("[WarmModelPool] Planned action: action="
                 << actionToString(static_cast<int>(pool_type))
                 << " model=" << action.model_id
                 << " available_mb=" << input.available_memory_mb
                 << " active_reserved="
                 << input.snapshot.active_reserved_models
                 << "/" << input.snapshot.max_active_models);
    }

    return actions;
}

void WarmModelPool::applyActions(const std::vector<PoolAction>& actions) {
    for (const PoolAction& action : actions) {
        if (!action.runtime) {
            continue;
        }

        if (action.type == PoolActionType::Activate) {
            LOG_INFO("[WarmModelPool] Applying action: activate model="
                     << action.model_id);
            if (!action.runtime->activate()) {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = runtimes_.find(action.model_id);
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
                     << action.model_id);
            action.runtime->failQueued(GenAIException(
                GenAIErrorCode::INSUFFICIENT_MEMORY,
                "Model '" + action.model_id +
                    "' cannot be admitted with current memory constraints",
                503));
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = runtimes_.find(action.model_id);
                if (it != runtimes_.end()) {
                    it->second.active_reserved = false;
                    it->second.eviction_requested = false;
                }
                event_pending_ = true;
            }
            cv_.notify_one();
            continue;
        }

        if (action.type == PoolActionType::ProtectedDrain) {
            LOG_INFO("[WarmModelPool] Applying action: protected_drain model="
                     << action.model_id);
            action.runtime->requestProtectedDrain();
            continue;
        }

        LOG_INFO("[WarmModelPool] Applying action: drain model="
                 << action.model_id);
        action.runtime->requestDrain();
    }
}

void WarmModelPool::handleRuntimeStateChanged(const std::string& model_id,
                                           ModelRuntimeState state) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = runtimes_.find(model_id);
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
                markResidentLocked(record, now);
                break;
            case ModelRuntimeState::Idle:
                record.idle_since = now;
                record.last_used_at = now;
                record.eviction_requested = false;
                markResidentLocked(record, now);
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
                record.expiry_pending = false;
                clearToolLeaseLocked(record);
                clearResidencyLocked(record);
                break;
        }

        event_pending_ = true;
        LOG_INFO("[WarmModelPool] Runtime state changed: model=" << model_id
                 << " " << stateToString(old_state)
                 << " -> " << stateToString(state)
                 << " active_reserved="
                 << (record.active_reserved ? "true" : "false")
                 << " eviction_requested="
                 << (record.eviction_requested ? "true" : "false")
                 << " tool_lease="
                 << (record.tool_lease_until ? "true" : "false"));
    }

    cv_.notify_one();
}

WarmModelPool::RuntimeRecord& WarmModelPool::getOrCreateRuntimeLocked(
    const std::string& model_id,
    std::chrono::steady_clock::time_point now) {
    auto existing = runtimes_.find(model_id);
    if (existing != runtimes_.end()) {
        return existing->second;
    }

    ModelRuntimeEvents events;
    events.on_state_changed =
        [this](const std::string& changed_model_id, ModelRuntimeState state) {
            handleRuntimeStateChanged(changed_model_id, state);
        };

    // Create backend + orchestrator pair via factory
    RuntimePair pair = runtime_factory_(model_id);

    auto runtime = std::make_unique<ModelRuntime>(
        model_id,
        std::move(pair.backend),
        std::move(pair.orchestrator),
        std::move(events),
        config_.running_cancel_mode);
    runtime->start();

    RuntimeRecord record;
    record.runtime = std::move(runtime);
    record.state = ModelRuntimeState::NotResident;
    record.active_reserved = false;
    record.eviction_requested = false;
    record.expiry_pending = false;
    record.last_used_at = now;

    auto inserted = runtimes_.emplace(model_id, std::move(record));
    LOG_INFO("[WarmModelPool] Created runtime: model=" << model_id);
    return inserted.first->second;
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
    return SystemResourceManager::getInstance().getAvailableMemoryMb();
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
    record.tool_chain_id.clear();
    record.tool_lease_until.reset();
}

void WarmModelPool::markResidentLocked(
    RuntimeRecord& record,
    std::chrono::steady_clock::time_point now) {
    if (!record.resident_since) {
        record.resident_since = now;

        if (config_.model_residency_ttl.count() > 0) {
            record.residency_expires_at = now + config_.model_residency_ttl;
        } else {
            record.residency_expires_at.reset();
        }
    }
}

void WarmModelPool::clearResidencyLocked(RuntimeRecord& record) {
    record.resident_since.reset();
    record.residency_expires_at.reset();
}

std::chrono::steady_clock::time_point
WarmModelPool::nextIdleDeadlineLocked() const {
    auto next_deadline = std::chrono::steady_clock::time_point::max();

    for (const auto& entry : runtimes_) {
        const RuntimeRecord& record = entry.second;
        if (record.tool_lease_until) {
            next_deadline = std::min(next_deadline, *record.tool_lease_until);
        }

        if (record.residency_expires_at &&
            !record.expiry_pending &&
            record.active_reserved &&
            isActiveReservedState(record.state)) {
            next_deadline = std::min(
                next_deadline,
                *record.residency_expires_at);
        }

        if (record.state != ModelRuntimeState::Idle ||
            !record.active_reserved ||
            record.eviction_requested ||
            record.tool_lease_until ||
            !record.idle_since ||
            runtimeHasQueuedWorkLocked(record)) {
            continue;
        }

        next_deadline = std::min(
            next_deadline,
            *record.idle_since + config_.idle_timeout);
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

} // namespace scheduler
