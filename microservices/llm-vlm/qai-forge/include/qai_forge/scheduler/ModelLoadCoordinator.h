// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>

namespace scheduler {

/** @brief Coordinates cross-pool model loads and DSP reclaim. */
class ModelLoadCoordinator {
public:
    class LoadReservation {
    public:
        LoadReservation() = default;
        ~LoadReservation();

        LoadReservation(const LoadReservation&) = delete;
        LoadReservation& operator=(const LoadReservation&) = delete;
        LoadReservation(LoadReservation&& other) noexcept;
        LoadReservation& operator=(LoadReservation&& other) noexcept;

        explicit operator bool() const noexcept;
        void release() noexcept;

    private:
        friend class ModelLoadCoordinator;

        LoadReservation(ModelLoadCoordinator* coordinator,
                        long memory_mb) noexcept;

        ModelLoadCoordinator* coordinator_ = nullptr;
        long memory_mb_ = 0;
    };

    class ReclaimGuard {
    public:
        ReclaimGuard() = default;
        ~ReclaimGuard();

        ReclaimGuard(const ReclaimGuard&) = delete;
        ReclaimGuard& operator=(const ReclaimGuard&) = delete;
        ReclaimGuard(ReclaimGuard&& other) noexcept;
        ReclaimGuard& operator=(ReclaimGuard&& other) noexcept;

        void release() noexcept;

    private:
        friend class ModelLoadCoordinator;

        explicit ReclaimGuard(ModelLoadCoordinator* coordinator) noexcept;

        ModelLoadCoordinator* coordinator_ = nullptr;
    };

    explicit ModelLoadCoordinator(long memory_headroom_mb);

    /** @brief Enable admission after scheduler startup. */
    void start();

    /** @brief Stop admission and wake waiting model loads. */
    void shutdown();

    /**
     * @brief Wait for stable memory and reserve one in-flight model load.
     * @param model_id Model being loaded, used for diagnostics.
     * @param memory_mb Estimated memory required by the model.
     * @return Move-only reservation released after load completion.
     */
    LoadReservation acquire(const std::string& model_id, long memory_mb);

    /**
     * @brief Reserve an in-flight model load without waiting.
     * @param model_id Model being considered for activation.
     * @param memory_mb Estimated memory required by the model.
     * @return Valid reservation when admission succeeds; otherwise empty.
     */
    LoadReservation tryAcquire(const std::string& model_id, long memory_mb);

    /** @brief Mark DSP unload/reclaim as active. */
    ReclaimGuard beginReclaim(const std::string& model_id);

    /**
     * @brief Atomically convert a failed load into DSP reclaim ownership.
     * @param model_id Model whose failed load is being cleaned up.
     * @param reservation Active reservation held by the failed load.
     * @return Guard that blocks new loads until reclaim finishes.
     */
    ReclaimGuard beginReclaim(const std::string& model_id,
                              LoadReservation&& reservation);

    /** @brief Return currently usable memory after in-flight reservations. */
    long availableMemoryMb() const;

    /** @brief Return whether an active load or DSP reclaim can change capacity. */
    bool isMemoryTransitionActive() const;

    /** @brief Install the scheduler wake callback for shared capacity changes. */
    void setCapacityChangedCallback(std::function<void()> callback);

private:
    LoadReservation reserveLoadLocked(const std::string& model_id,
                                      long required_memory_mb);
    void releaseLoad(long memory_mb) noexcept;
    void endReclaim() noexcept;
    void notifyCapacityChanged() noexcept;
    ReclaimGuard beginReclaimImpl(const std::string& model_id,
                                  LoadReservation* reservation);
    long availableMemoryMbLocked() const;

    const long memory_headroom_mb_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    long reserved_load_memory_mb_ = 0;
    size_t active_load_count_ = 0;
    size_t reclaim_count_ = 0;
    bool shutdown_requested_ = false;
    std::function<void()> capacity_changed_callback_;
};

} // namespace scheduler
