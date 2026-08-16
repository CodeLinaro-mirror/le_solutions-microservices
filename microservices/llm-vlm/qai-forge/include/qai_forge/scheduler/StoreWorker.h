// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>

namespace scheduler {

// Bounded idempotent hand-off queue for consumer-owned persistence work. It has
// no model backend and never controls memory readiness or runtime state.
class StoreWorker {
public:
    explicit StoreWorker(size_t max_queue_depth = 256);
    ~StoreWorker();

    StoreWorker(const StoreWorker&) = delete;
    StoreWorker& operator=(const StoreWorker&) = delete;

    void start();
    bool enqueue(std::string idempotency_key, std::function<void()> task);
    void stop(bool force);

private:
    struct Task {
        std::string idempotency_key;
        std::function<void()> persist;
    };

    void workerLoop();

    const size_t max_queue_depth_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_thread_;
    std::deque<Task> queue_;
    std::unordered_set<std::string> accepted_keys_;
    bool started_ = false;
    bool stop_requested_ = false;
    bool force_stop_ = false;
};

} // namespace scheduler
