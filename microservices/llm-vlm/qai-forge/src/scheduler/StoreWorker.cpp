// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/scheduler/StoreWorker.h"

#include "qai_forge/utils/Logger.h"

#include <exception>
#include <utility>

namespace scheduler {

StoreWorker::StoreWorker(size_t max_queue_depth)
    : max_queue_depth_(max_queue_depth) {}

StoreWorker::~StoreWorker() {
    stop(false);
}

void StoreWorker::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
        return;
    }
    stop_requested_ = false;
    force_stop_ = false;
    worker_thread_ = std::thread(&StoreWorker::workerLoop, this);
    started_ = true;
}

bool StoreWorker::enqueue(std::string idempotency_key,
                          std::function<void()> task) {
    if (idempotency_key.empty() || !task) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || stop_requested_ ||
            queue_.size() >= max_queue_depth_ ||
            accepted_keys_.count(idempotency_key) > 0) {
            return false;
        }
        accepted_keys_.insert(idempotency_key);
        queue_.push_back(Task{
            std::move(idempotency_key),
            std::move(task),
        });
    }
    cv_.notify_one();
    return true;
}

void StoreWorker::stop(bool force) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            return;
        }
        stop_requested_ = true;
        force_stop_ = force_stop_ || force;
        if (force_stop_) {
            queue_.clear();
        }
    }
    cv_.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    accepted_keys_.clear();
    started_ = false;
}

void StoreWorker::workerLoop() {
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() {
                return stop_requested_ || !queue_.empty();
            });
            if (queue_.empty()) {
                if (stop_requested_) {
                    break;
                }
                continue;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
        }

        try {
            task.persist();
        } catch (const std::exception& error) {
            LOG_WARN("[StoreWorker] Persistence hand-off failed: key="
                     << task.idempotency_key << " message=\""
                     << error.what() << "\"");
        } catch (...) {
            LOG_WARN("[StoreWorker] Persistence hand-off failed: key="
                     << task.idempotency_key << " message=<unknown>");
        }

        std::lock_guard<std::mutex> lock(mutex_);
        accepted_keys_.erase(task.idempotency_key);
    }
}

} // namespace scheduler
