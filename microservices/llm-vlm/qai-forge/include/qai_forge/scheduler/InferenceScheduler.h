// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/backend/BackendFactory.h"
#include "qai_forge/scheduler/CancelResult.h"
#include "qai_forge/scheduler/GenerativeJob.h"
#include "qai_forge/scheduler/PredictiveModelPool.h"
#include "qai_forge/scheduler/StoreWorker.h"
#include "qai_forge/scheduler/WarmModelPool.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace scheduler {

struct GenerativeScheduleMetadata {
    std::string job_id;
    std::string model_id;
    JobPriority priority = JobPriority::NEW_REQUEST;
};

struct PredictiveScheduleMetadata {
    std::string job_id;
    std::string model_id;
};

inline WarmModelPoolConfig defaultPredictivePoolConfig() {
    WarmModelPoolConfig config;
    config.max_queue_depth_per_model = 12;
    return config;
}

struct InferenceSchedulerConfig {
    WarmModelPoolConfig generative_pool_config;
    WarmModelPoolConfig predictive_pool_config = defaultPredictivePoolConfig();
    size_t max_concurrent_model_loads = 1;
};

class GenerativeRuntimeHandle {
public:
    GenerativeRuntimeHandle() = default;
    ~GenerativeRuntimeHandle();

    GenerativeRuntimeHandle(const GenerativeRuntimeHandle&) = delete;
    GenerativeRuntimeHandle& operator=(const GenerativeRuntimeHandle&) = delete;
    GenerativeRuntimeHandle(GenerativeRuntimeHandle&& other) noexcept;
    GenerativeRuntimeHandle& operator=(GenerativeRuntimeHandle&& other) noexcept;

    SubmitResult submit(GenerativeJobPtr job);
    bool valid() const;

private:
    friend class InferenceScheduler;

    GenerativeRuntimeHandle(WarmModelPool* pool,
                            ModelRuntime* runtime,
                            GenerativeScheduleMetadata metadata) noexcept;
    void release() noexcept;

    WarmModelPool* pool_ = nullptr;
    ModelRuntime* runtime_ = nullptr;
    GenerativeScheduleMetadata metadata_;
    bool active_ = false;
};

class PredictiveRuntimeHandle {
public:
    PredictiveRuntimeHandle() = default;
    ~PredictiveRuntimeHandle();

    PredictiveRuntimeHandle(const PredictiveRuntimeHandle&) = delete;
    PredictiveRuntimeHandle& operator=(const PredictiveRuntimeHandle&) = delete;
    PredictiveRuntimeHandle(PredictiveRuntimeHandle&& other) noexcept;
    PredictiveRuntimeHandle& operator=(PredictiveRuntimeHandle&& other) noexcept;

    SubmitResult submit(PredictiveJobPtr job);
    bool valid() const;

private:
    friend class InferenceScheduler;

    PredictiveRuntimeHandle(PredictiveModelPool* pool,
                            PredictiveModelRuntime* runtime,
                            PredictiveScheduleMetadata metadata) noexcept;
    void release() noexcept;

    PredictiveModelPool* pool_ = nullptr;
    PredictiveModelRuntime* runtime_ = nullptr;
    PredictiveScheduleMetadata metadata_;
    bool active_ = false;
};

// Admits typed inference metadata and returns a move-only runtime reservation.
// Request payload construction and callback semantics remain in QaiForge.
class InferenceScheduler {
public:
    explicit InferenceScheduler(
        InferenceSchedulerConfig config = {},
        ModelRuntimePairFactory generative_factory = {},
        PredictiveBackendFactory predictive_factory = {});
    ~InferenceScheduler();

    InferenceScheduler(const InferenceScheduler&) = delete;
    InferenceScheduler& operator=(const InferenceScheduler&) = delete;
    InferenceScheduler(InferenceScheduler&&) = delete;
    InferenceScheduler& operator=(InferenceScheduler&&) = delete;

    static InferenceScheduler& getInstance();

    void start();
    GenerativeRuntimeHandle reserve(
        const GenerativeScheduleMetadata& metadata);
    PredictiveRuntimeHandle reserve(
        const PredictiveScheduleMetadata& metadata);
    std::shared_ptr<ConversationMemoryCoordinator> memoryCoordinator() const;
    std::optional<ConversationMemoryUpdate> awaitConversationMemory(
        const std::string& memory_key);
    bool enqueueStoreTask(std::string idempotency_key,
                          std::function<void()> task);
    CancelResult cancel(const std::string& job_id);

    bool openToolLease(const std::string& model_id,
                       const std::string& chain_id,
                       std::chrono::milliseconds ttl);
    bool renewToolLease(const std::string& model_id,
                        const std::string& chain_id,
                        std::chrono::milliseconds ttl);
    bool closeToolLease(const std::string& model_id,
                        const std::string& chain_id);

    void shutdown(bool force = false);
    void stop(bool force = false);

private:
    static ModelRuntimeEvents loadEvents(InferenceScheduler* scheduler);
    void acquireLoadPermit();
    void releaseLoadPermit() noexcept;

    InferenceSchedulerConfig config_;
    size_t max_concurrent_model_loads_ = 1;

    mutable std::mutex mutex_;
    std::condition_variable load_cv_;
    size_t active_model_loads_ = 0;
    bool started_ = false;
    bool shutdown_requested_ = false;

    std::shared_ptr<ConversationMemoryCoordinator> memory_coordinator_;
    StoreWorker store_worker_;
    WarmModelPool generative_pool_;
    PredictiveModelPool predictive_pool_;
};

} // namespace scheduler
