// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/scheduler/InferenceScheduler.h"

#include "qai_forge/utils/Logger.h"

#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <utility>

namespace scheduler {

namespace {

size_t parseSizeEnv(const char* name, size_t fallback) {
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }

    try {
        const unsigned long long parsed = std::stoull(value);
        if (parsed > static_cast<unsigned long long>(
                         std::numeric_limits<size_t>::max())) {
            return fallback;
        }
        return static_cast<size_t>(parsed);
    } catch (...) {
        return fallback;
    }
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

std::chrono::milliseconds parseSecondsEnv(
    const char* name,
    std::chrono::milliseconds fallback) {
    const long parsed = parseLongEnv(name, -1);
    return parsed > 0 ? std::chrono::seconds(parsed) : fallback;
}

InferenceSchedulerConfig normalizeConfig(InferenceSchedulerConfig config) {
    if (config.max_concurrent_model_loads == 0) {
        config.max_concurrent_model_loads = 1;
    }
    return config;
}

InferenceSchedulerConfig configFromEnvironment() {
    InferenceSchedulerConfig config;
    const size_t max_active_models = parseSizeEnv(
        "MAX_ACTIVE_MODELS",
        config.generative_pool_config.max_active_models);
    config.generative_pool_config.max_active_models = max_active_models;
    config.predictive_pool_config.max_active_models = max_active_models;

    const auto blocked_timeout = parseSecondsEnv(
        "BLOCKED_ADMISSION_TIMEOUT_SECONDS",
        config.generative_pool_config.blocked_admission_timeout);
    config.generative_pool_config.blocked_admission_timeout = blocked_timeout;
    config.predictive_pool_config.blocked_admission_timeout = blocked_timeout;

    const auto tool_timeout = parseSecondsEnv(
        "TOOL_RESPONSE_TIMEOUT_SECONDS",
        config.generative_pool_config.tool_response_timeout);
    config.generative_pool_config.tool_response_timeout = tool_timeout;
    config.predictive_pool_config.max_queue_depth_per_model = parseSizeEnv(
        "MAX_QUEUE_DEPTH_PER_MODEL",
        config.predictive_pool_config.max_queue_depth_per_model);
    config.max_concurrent_model_loads = parseSizeEnv(
        "MAX_CONCURRENT_MODEL_LOADS",
        config.max_concurrent_model_loads);
    return normalizeConfig(std::move(config));
}

void validateMetadata(const std::string& job_id,
                      const std::string& model_id) {
    if (job_id.empty() || model_id.empty()) {
        throw GenAIException(
            GenAIErrorCode::INVALID_REQUEST,
            "Schedule metadata requires non-empty job_id and model_id",
            400);
    }
}

} // namespace

GenerativeRuntimeHandle::GenerativeRuntimeHandle(
    WarmModelPool* pool,
    ModelRuntime* runtime,
    GenerativeScheduleMetadata metadata) noexcept
    : pool_(pool),
      runtime_(runtime),
      metadata_(std::move(metadata)),
      active_(pool_ && runtime_) {}

GenerativeRuntimeHandle::~GenerativeRuntimeHandle() {
    release();
}

GenerativeRuntimeHandle::GenerativeRuntimeHandle(
    GenerativeRuntimeHandle&& other) noexcept
    : pool_(other.pool_),
      runtime_(other.runtime_),
      metadata_(std::move(other.metadata_)),
      active_(other.active_) {
    other.pool_ = nullptr;
    other.runtime_ = nullptr;
    other.active_ = false;
}

GenerativeRuntimeHandle& GenerativeRuntimeHandle::operator=(
    GenerativeRuntimeHandle&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    release();
    pool_ = other.pool_;
    runtime_ = other.runtime_;
    metadata_ = std::move(other.metadata_);
    active_ = other.active_;
    other.pool_ = nullptr;
    other.runtime_ = nullptr;
    other.active_ = false;
    return *this;
}

SubmitResult GenerativeRuntimeHandle::submit(GenerativeJobPtr job) {
    if (!valid()) {
        throw std::logic_error("Generative runtime reservation is inactive");
    }
    if (!job || job->model_id != metadata_.model_id) {
        release();
        throw GenAIException(
            GenAIErrorCode::INVALID_REQUEST,
            "Generative job model does not match reserved runtime model '" +
                metadata_.model_id + "'",
            400);
    }

    try {
        SubmitResult result = pool_->submit(runtime_, std::move(job));
        release();
        return result;
    } catch (...) {
        release();
        throw;
    }
}

bool GenerativeRuntimeHandle::valid() const {
    return active_ && pool_ && runtime_;
}

void GenerativeRuntimeHandle::release() noexcept {
    if (!active_) {
        return;
    }
    active_ = false;
    if (pool_) {
        pool_->releaseReservation(runtime_);
    }
    pool_ = nullptr;
    runtime_ = nullptr;
}

PredictiveRuntimeHandle::PredictiveRuntimeHandle(
    PredictiveModelPool* pool,
    PredictiveModelRuntime* runtime,
    PredictiveScheduleMetadata metadata) noexcept
    : pool_(pool),
      runtime_(runtime),
      metadata_(std::move(metadata)),
      active_(pool_ && runtime_) {}

PredictiveRuntimeHandle::~PredictiveRuntimeHandle() {
    release();
}

PredictiveRuntimeHandle::PredictiveRuntimeHandle(
    PredictiveRuntimeHandle&& other) noexcept
    : pool_(other.pool_),
      runtime_(other.runtime_),
      metadata_(std::move(other.metadata_)),
      active_(other.active_) {
    other.pool_ = nullptr;
    other.runtime_ = nullptr;
    other.active_ = false;
}

PredictiveRuntimeHandle& PredictiveRuntimeHandle::operator=(
    PredictiveRuntimeHandle&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    release();
    pool_ = other.pool_;
    runtime_ = other.runtime_;
    metadata_ = std::move(other.metadata_);
    active_ = other.active_;
    other.pool_ = nullptr;
    other.runtime_ = nullptr;
    other.active_ = false;
    return *this;
}

SubmitResult PredictiveRuntimeHandle::submit(PredictiveJobPtr job) {
    if (!valid()) {
        throw std::logic_error("Predictive runtime reservation is inactive");
    }
    if (!job || job->model_id != metadata_.model_id) {
        release();
        throw GenAIException(
            GenAIErrorCode::INVALID_REQUEST,
            "Predictive job model does not match reserved runtime model '" +
                metadata_.model_id + "'",
            400);
    }

    try {
        SubmitResult result = pool_->submit(runtime_, std::move(job));
        release();
        return result;
    } catch (...) {
        release();
        throw;
    }
}

bool PredictiveRuntimeHandle::valid() const {
    return active_ && pool_ && runtime_;
}

void PredictiveRuntimeHandle::release() noexcept {
    if (!active_) {
        return;
    }
    active_ = false;
    if (pool_) {
        pool_->releaseReservation(runtime_);
    }
    pool_ = nullptr;
    runtime_ = nullptr;
}

InferenceScheduler::InferenceScheduler(
    InferenceSchedulerConfig config,
    ModelRuntimePairFactory generative_factory,
    PredictiveBackendFactory predictive_factory)
    : config_(normalizeConfig(std::move(config))),
      max_concurrent_model_loads_(config_.max_concurrent_model_loads),
      generative_pool_(
          config_.generative_pool_config,
          std::move(generative_factory),
          loadEvents(this)),
      predictive_pool_(
          config_.predictive_pool_config,
          std::move(predictive_factory),
          loadEvents(this)) {
    LOG_INFO("[InferenceScheduler] Configured: max_concurrent_model_loads="
             << max_concurrent_model_loads_);
}

ModelRuntimeEvents InferenceScheduler::loadEvents(
    InferenceScheduler* scheduler) {
    ModelRuntimeEvents events;
    events.acquire_load_permit = [scheduler]() {
        scheduler->acquireLoadPermit();
    };
    events.release_load_permit = [scheduler]() {
        scheduler->releaseLoadPermit();
    };
    return events;
}

InferenceScheduler::~InferenceScheduler() {
    shutdown(false);
}

InferenceScheduler& InferenceScheduler::getInstance() {
    static InferenceScheduler instance(configFromEnvironment());
    return instance;
}

void InferenceScheduler::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
        return;
    }
    shutdown_requested_ = false;
    generative_pool_.start();
    started_ = true;
    LOG_INFO("[InferenceScheduler] Started");
}

GenerativeRuntimeHandle InferenceScheduler::reserve(
    const GenerativeScheduleMetadata& metadata) {
    validateMetadata(metadata.job_id, metadata.model_id);
    GenerativeScheduleMetadata stored_metadata = metadata;
    ModelRuntime* runtime = generative_pool_.reserve(metadata.model_id);
    return GenerativeRuntimeHandle(
        &generative_pool_,
        runtime,
        std::move(stored_metadata));
}

PredictiveRuntimeHandle InferenceScheduler::reserve(
    const PredictiveScheduleMetadata& metadata) {
    validateMetadata(metadata.job_id, metadata.model_id);
    PredictiveScheduleMetadata stored_metadata = metadata;
    PredictiveModelRuntime* runtime = predictive_pool_.reserve(metadata.model_id);
    return PredictiveRuntimeHandle(
        &predictive_pool_,
        runtime,
        std::move(stored_metadata));
}

CancelResult InferenceScheduler::cancel(const std::string& job_id) {
    return generative_pool_.cancel(job_id);
}

bool InferenceScheduler::openToolLease(
    const std::string& model_id,
    const std::string& chain_id,
    std::chrono::milliseconds ttl) {
    return generative_pool_.openToolLease(model_id, chain_id, ttl);
}

bool InferenceScheduler::renewToolLease(
    const std::string& model_id,
    const std::string& chain_id,
    std::chrono::milliseconds ttl) {
    return generative_pool_.renewToolLease(model_id, chain_id, ttl);
}

bool InferenceScheduler::closeToolLease(const std::string& model_id,
                                        const std::string& chain_id) {
    return generative_pool_.closeToolLease(model_id, chain_id);
}

void InferenceScheduler::shutdown(bool force) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_requested_ = true;
    }
    load_cv_.notify_all();

    generative_pool_.stop(force);
    predictive_pool_.stop(force);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        started_ = false;
    }
    LOG_INFO("[InferenceScheduler] Stopped force="
             << (force ? "true" : "false"));
}

void InferenceScheduler::stop(bool force) {
    shutdown(force);
}

void InferenceScheduler::acquireLoadPermit() {
    std::unique_lock<std::mutex> lock(mutex_);
    load_cv_.wait(lock, [this]() {
        return shutdown_requested_ ||
               active_model_loads_ < max_concurrent_model_loads_;
    });
    if (shutdown_requested_) {
        throw GenAIException(
            GenAIErrorCode::HARDWARE_UNAVAILABLE,
            "Inference scheduler is shutting down before model load",
            503);
    }
    ++active_model_loads_;
}

void InferenceScheduler::releaseLoadPermit() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_model_loads_ > 0) {
            --active_model_loads_;
        }
    }
    load_cv_.notify_one();
}

} // namespace scheduler
