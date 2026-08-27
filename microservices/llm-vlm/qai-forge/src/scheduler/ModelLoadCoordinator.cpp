// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/scheduler/ModelLoadCoordinator.h"

#include "qai_forge/InternalDTOs.h"
#include "qai_forge/managers/SystemResourceManager.h"
#include "qai_forge/utils/Logger.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace scheduler {

ModelLoadCoordinator::LoadReservation::LoadReservation(
    ModelLoadCoordinator* coordinator,
    long memory_mb) noexcept
    : coordinator_(coordinator), memory_mb_(memory_mb) {}

ModelLoadCoordinator::LoadReservation::~LoadReservation() {
    release();
}

ModelLoadCoordinator::LoadReservation::LoadReservation(
    LoadReservation&& other) noexcept
    : coordinator_(other.coordinator_), memory_mb_(other.memory_mb_) {
    other.coordinator_ = nullptr;
    other.memory_mb_ = 0;
}

ModelLoadCoordinator::LoadReservation&
ModelLoadCoordinator::LoadReservation::operator=(
    LoadReservation&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    release();
    coordinator_ = other.coordinator_;
    memory_mb_ = other.memory_mb_;
    other.coordinator_ = nullptr;
    other.memory_mb_ = 0;
    return *this;
}

ModelLoadCoordinator::LoadReservation::operator bool() const noexcept {
    return coordinator_ != nullptr;
}

void ModelLoadCoordinator::LoadReservation::release() noexcept {
    if (!coordinator_) {
        return;
    }
    coordinator_->releaseLoad(memory_mb_);
    coordinator_ = nullptr;
    memory_mb_ = 0;
}

ModelLoadCoordinator::ReclaimGuard::ReclaimGuard(
    ModelLoadCoordinator* coordinator) noexcept
    : coordinator_(coordinator) {}

ModelLoadCoordinator::ReclaimGuard::~ReclaimGuard() {
    release();
}

ModelLoadCoordinator::ReclaimGuard::ReclaimGuard(
    ReclaimGuard&& other) noexcept
    : coordinator_(other.coordinator_) {
    other.coordinator_ = nullptr;
}

ModelLoadCoordinator::ReclaimGuard&
ModelLoadCoordinator::ReclaimGuard::operator=(ReclaimGuard&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    release();
    coordinator_ = other.coordinator_;
    other.coordinator_ = nullptr;
    return *this;
}

void ModelLoadCoordinator::ReclaimGuard::release() noexcept {
    if (!coordinator_) {
        return;
    }
    coordinator_->endReclaim();
    coordinator_ = nullptr;
}

ModelLoadCoordinator::ModelLoadCoordinator(long memory_headroom_mb)
    : memory_headroom_mb_(std::max(0L, memory_headroom_mb)) {}

void ModelLoadCoordinator::start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_requested_ = false;
    }
    cv_.notify_all();
}

void ModelLoadCoordinator::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_requested_ = true;
    }
    cv_.notify_all();
}

ModelLoadCoordinator::LoadReservation ModelLoadCoordinator::acquire(
    const std::string& model_id,
    long memory_mb) {
    const long required_memory_mb = std::max(1L, memory_mb);
    std::unique_lock<std::mutex> lock(mutex_);

    while (!shutdown_requested_) {
        const long available_memory_mb = availableMemoryMbLocked();
        if (reclaim_count_ == 0 &&
            available_memory_mb >=
                required_memory_mb + memory_headroom_mb_) {
            return reserveLoadLocked(model_id, required_memory_mb);
        }

        if (reclaim_count_ == 0 && active_load_count_ == 0) {
            throw GenAIException(
                GenAIErrorCode::INSUFFICIENT_MEMORY,
                "Cannot reserve memory for model '" + model_id +
                    "': available=" +
                    std::to_string(available_memory_mb) +
                    "MB, required=" +
                    std::to_string(required_memory_mb) +
                    "MB, headroom=" +
                    std::to_string(memory_headroom_mb_) + "MB",
                503);
        }

        cv_.wait_for(lock, std::chrono::milliseconds(100));
    }

    throw GenAIException(
        GenAIErrorCode::HARDWARE_UNAVAILABLE,
        "Model load cancelled while the inference scheduler is shutting down",
        503);
}

ModelLoadCoordinator::LoadReservation ModelLoadCoordinator::tryAcquire(
    const std::string& model_id,
    long memory_mb) {
    const long required_memory_mb = std::max(1L, memory_mb);
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_requested_ || reclaim_count_ > 0 ||
        availableMemoryMbLocked() <
            required_memory_mb + memory_headroom_mb_) {
        return {};
    }
    return reserveLoadLocked(model_id, required_memory_mb);
}

ModelLoadCoordinator::ReclaimGuard ModelLoadCoordinator::beginReclaim(
    const std::string& model_id) {
    return beginReclaimImpl(model_id, nullptr);
}

ModelLoadCoordinator::ReclaimGuard ModelLoadCoordinator::beginReclaim(
    const std::string& model_id,
    LoadReservation&& reservation) {
    return beginReclaimImpl(model_id, &reservation);
}

ModelLoadCoordinator::ReclaimGuard ModelLoadCoordinator::beginReclaimImpl(
    const std::string& model_id,
    LoadReservation* reservation) {
    std::unique_lock<std::mutex> lock(mutex_);
    ++reclaim_count_;

    if (reservation && reservation->coordinator_ == this) {
        reserved_load_memory_mb_ = std::max(
            0L,
            reserved_load_memory_mb_ -
                std::max(0L, reservation->memory_mb_));
        if (active_load_count_ > 0) {
            --active_load_count_;
        }
        reservation->coordinator_ = nullptr;
        reservation->memory_mb_ = 0;
    }

    LOG_INFO("[ModelLoadCoordinator] DSP reclaim pending: model="
             << model_id << " active_reclaims=" << reclaim_count_
             << " active_loads=" << active_load_count_);
    cv_.notify_all();
    cv_.wait(lock, [this]() {
        return active_load_count_ == 0;
    });

    LOG_INFO("[ModelLoadCoordinator] DSP reclaim started: model="
             << model_id << " active_reclaims=" << reclaim_count_);
    return ReclaimGuard(this);
}

long ModelLoadCoordinator::availableMemoryMb() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return availableMemoryMbLocked();
}

bool ModelLoadCoordinator::isMemoryTransitionActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_load_count_ > 0 || reclaim_count_ > 0;
}

void ModelLoadCoordinator::setCapacityChangedCallback(
    std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    capacity_changed_callback_ = std::move(callback);
}

ModelLoadCoordinator::LoadReservation
ModelLoadCoordinator::reserveLoadLocked(const std::string& model_id,
                                        long required_memory_mb) {
    const long available_memory_mb = availableMemoryMbLocked();
    reserved_load_memory_mb_ += required_memory_mb;
    ++active_load_count_;
    LOG_INFO("[ModelLoadCoordinator] Reserved load memory: model="
             << model_id << " required_mb=" << required_memory_mb
             << " reserved_mb=" << reserved_load_memory_mb_
             << " active_loads=" << active_load_count_
             << " available_mb=" << available_memory_mb
             << " headroom_mb=" << memory_headroom_mb_);
    return LoadReservation(this, required_memory_mb);
}

void ModelLoadCoordinator::releaseLoad(long memory_mb) noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reserved_load_memory_mb_ =
            std::max(0L, reserved_load_memory_mb_ - std::max(0L, memory_mb));
        if (active_load_count_ > 0) {
            --active_load_count_;
        }
    }
    cv_.notify_all();
    notifyCapacityChanged();
}

void ModelLoadCoordinator::endReclaim() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (reclaim_count_ > 0) {
            --reclaim_count_;
        }
        LOG_INFO("[ModelLoadCoordinator] DSP reclaim finished: active_reclaims="
                 << reclaim_count_);
    }
    cv_.notify_all();
    notifyCapacityChanged();
}

void ModelLoadCoordinator::notifyCapacityChanged() noexcept {
    std::function<void()> callback;
    try {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = capacity_changed_callback_;
        }
        if (callback) {
            callback();
        }
    } catch (...) {
    }
}

long ModelLoadCoordinator::availableMemoryMbLocked() const {
    const long system_available_mb =
        SystemResourceManager::getInstance().getAvailableMemoryMb();
    return std::max(0L, system_available_mb - reserved_load_memory_mb_);
}

} // namespace scheduler
