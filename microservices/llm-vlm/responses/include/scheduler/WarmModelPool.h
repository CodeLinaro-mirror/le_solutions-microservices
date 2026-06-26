// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "scheduler/CancelResult.h"
#include "scheduler/ModelRuntime.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class IGenerativeBackend;

namespace scheduler {

class EvictionPolicy;

struct WarmModelPoolConfig {
    size_t max_active_models = 3;
    size_t max_concurrent_model_loads = 1;
    std::chrono::milliseconds idle_timeout = std::chrono::minutes(5);
    std::chrono::milliseconds model_residency_ttl = std::chrono::seconds(60);
    std::chrono::milliseconds tool_response_timeout = std::chrono::seconds(30);
    size_t max_queue_depth_per_model = 0; // 0 = unlimited
    long memory_headroom_mb = 1024;
    RunningCancelMode running_cancel_mode = RunningCancelMode::SOFT;
};

struct ModelPoolRuntimeSnapshot {
    std::string model_id;
    ModelRuntimeState state = ModelRuntimeState::NotResident;
    QueueSnapshot queue;
    QueueAdmissionCandidate candidate;
    bool has_running_job = false;
    std::string running_job_id;
    bool healthy = false;
    bool active_reserved = false;
    bool eviction_requested = false;
    bool expiry_pending = false;
    bool tool_lease_active = false;
    std::string tool_chain_id;
    std::optional<std::chrono::steady_clock::time_point> tool_lease_until;
    std::optional<std::chrono::steady_clock::time_point> resident_since;
    std::optional<std::chrono::steady_clock::time_point> residency_expires_at;
    std::chrono::steady_clock::time_point last_used_at;
    std::optional<std::chrono::steady_clock::time_point> idle_since;
};

struct ModelPoolSnapshot {
    size_t max_active_models = 0;
    size_t active_reserved_models = 0;
    size_t runtime_count = 0;
    std::vector<ModelPoolRuntimeSnapshot> runtimes;
};

using ModelBackendFactory =
    std::function<std::unique_ptr<IGenerativeBackend>(const std::string&)>;

// Owns warm model residency. It coordinates which ModelRuntime may load/unload,
// but ModelRuntime still owns actual per-model execution.
class WarmModelPool {
public:
    explicit WarmModelPool(WarmModelPoolConfig config,
                           ModelBackendFactory backend_factory = {});
    ~WarmModelPool();

    WarmModelPool(const WarmModelPool&) = delete;
    WarmModelPool& operator=(const WarmModelPool&) = delete;
    WarmModelPool(WarmModelPool&&) = delete;
    WarmModelPool& operator=(WarmModelPool&&) = delete;

    void start();
    SubmitResult submit(InferenceJobPtr job);
    CancelResult cancel(const std::string& job_id);
    bool openToolLease(const std::string& model_id,
                       const std::string& chain_id,
                       std::chrono::milliseconds ttl);
    bool renewToolLease(const std::string& model_id,
                        const std::string& chain_id,
                        std::chrono::milliseconds ttl);
    bool closeToolLease(const std::string& model_id,
                        const std::string& chain_id);
    void checkpoint();
    ModelPoolSnapshot snapshot() const;
    void stop(bool force = false);

private:
    struct RuntimeRecord {
        std::unique_ptr<ModelRuntime> runtime;
        ModelRuntimeState state = ModelRuntimeState::NotResident;
        bool active_reserved = false;
        bool eviction_requested = false;
        bool expiry_pending = false;
        std::string tool_chain_id;
        std::optional<std::chrono::steady_clock::time_point> tool_lease_until;
        std::optional<std::chrono::steady_clock::time_point> resident_since;
        std::optional<std::chrono::steady_clock::time_point> residency_expires_at;
        std::chrono::steady_clock::time_point last_used_at =
            std::chrono::steady_clock::now();
        std::optional<std::chrono::steady_clock::time_point> idle_since;
    };

    enum class PoolActionType {
        Activate,
        Drain,
        ProtectedDrain,
        Reject
    };

    struct PoolAction {
        PoolActionType type;
        std::string model_id;
        ModelRuntime* runtime = nullptr;
    };

    void eventLoop();
    std::vector<PoolAction> planActionsLocked(
        std::chrono::steady_clock::time_point now);
    void applyActions(const std::vector<PoolAction>& actions);
    ModelPoolSnapshot snapshotLocked() const;
    void refreshTimedStateLocked(std::chrono::steady_clock::time_point now);
    void handleRuntimeStateChanged(const std::string& model_id,
                                ModelRuntimeState state);

    RuntimeRecord& getOrCreateRuntimeLocked(
        const std::string& model_id,
        std::chrono::steady_clock::time_point now);
    size_t activeReservedCountLocked() const;
    long availableMemoryMb() const;
    long modelMemoryMb(const std::string& model_id) const;
    bool runtimeHasQueuedWorkLocked(const RuntimeRecord& record) const;
    void clearToolLeaseLocked(RuntimeRecord& record);
    void markResidentLocked(RuntimeRecord& record,
                            std::chrono::steady_clock::time_point now);
    void clearResidencyLocked(RuntimeRecord& record);
    std::chrono::steady_clock::time_point nextIdleDeadlineLocked() const;

    static bool isActiveReservedState(ModelRuntimeState state);

    WarmModelPoolConfig config_;
    ModelBackendFactory backend_factory_;
    std::unique_ptr<EvictionPolicy> eviction_policy_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread event_thread_;
    bool started_ = false;
    bool shutdown_requested_ = false;
    bool event_pending_ = false;

    std::unordered_map<std::string, RuntimeRecord> runtimes_;
};

} // namespace scheduler
