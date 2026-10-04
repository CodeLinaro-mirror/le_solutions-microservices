// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/scheduler/PostTurnWorker.h"

#include "qai_forge/backend/BackendAdapter.h"
#include "qai_forge/orchestration/OrchestratorAdapter.h"
#include "qai_forge/utils/Logger.h"

#include <utility>

namespace scheduler {

PostTurnWorker::PostTurnWorker(
    std::string model_id,
    BackendAdapter& backend,
    std::function<void(const std::string& session_id)> finish_post_turn,
    size_t max_queue_depth)
    : model_id_(std::move(model_id)),
      backend_(backend),
      finish_post_turn_(std::move(finish_post_turn)),
      max_queue_depth_(max_queue_depth) {}

PostTurnWorker::~PostTurnWorker() {
    stop(true);
}

void PostTurnWorker::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
        return;
    }
    stop_requested_ = false;
    force_stop_ = false;
    worker_thread_ = std::thread(&PostTurnWorker::workerLoop, this);
    started_ = true;
}

bool PostTurnWorker::enqueue(std::unique_ptr<PostTurnWork> work) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || stop_requested_ ||
            queue_.size() >= max_queue_depth_) {
            return false;
        }
        queue_.push_back(std::move(work));
    }
    cv_.notify_one();
    return true;
}

void PostTurnWorker::setMaxQueueDepth(size_t max_queue_depth) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_queue_depth_ = max_queue_depth;
}

void PostTurnWorker::stop(bool force) {
    std::deque<std::unique_ptr<PostTurnWork>> cancelled;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            return;
        }
        stop_requested_ = true;
        force_stop_ = force_stop_ || force;
        if (force_stop_) {
            cancelled.swap(queue_);
        }
    }

    for (auto& work : cancelled) {
        work->cancel();
        if (finish_post_turn_) {
            finish_post_turn_(work->sessionId());
        }
    }
    cv_.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
}

void PostTurnWorker::workerLoop() {
    while (true) {
        std::unique_ptr<PostTurnWork> work;
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
            work = std::move(queue_.front());
            queue_.pop_front();
        }

        // PostTurnWork::run() never throws — any orchestrator-specific
        // failure handling (e.g. publishing a Failed outcome to a memory
        // coordinator) is the concrete implementation's responsibility.
        // If a force-stop was requested after this item was popped (but
        // before it started running), prefer cancel() over run().
        if (forceRequested()) {
            work->cancel();
        } else {
            work->run(backend_);
        }

        if (finish_post_turn_) {
            finish_post_turn_(work->sessionId());
        }
    }
}

bool PostTurnWorker::forceRequested() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return force_stop_;
}

} // namespace scheduler
