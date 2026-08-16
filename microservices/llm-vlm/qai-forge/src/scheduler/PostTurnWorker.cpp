// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/scheduler/PostTurnWorker.h"

#include "qai_forge/backend/IGenerativeBackend.h"
#include "qai_forge/orchestration/IGenerativeOrchestrator.h"
#include "qai_forge/utils/Logger.h"

#include <optional>
#include <utility>

namespace scheduler {

PostTurnWorker::PostTurnWorker(
    std::string model_id,
    IGenerativeBackend& backend,
    std::shared_ptr<IGenerativeOrchestrator> orchestrator,
    std::shared_ptr<ConversationMemoryCoordinator> coordinator,
    std::function<void()> finish_post_turn,
    size_t max_queue_depth)
    : model_id_(std::move(model_id)),
      backend_(backend),
      orchestrator_(std::move(orchestrator)),
      coordinator_(std::move(coordinator)),
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

bool PostTurnWorker::enqueue(PostTurnTask task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || stop_requested_ ||
            queue_.size() >= max_queue_depth_) {
            return false;
        }
        queue_.push_back(std::move(task));
    }
    cv_.notify_one();
    return true;
}

void PostTurnWorker::stop(bool force) {
    std::deque<PostTurnTask> cancelled;
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

    for (PostTurnTask& task : cancelled) {
        cancelTask(task);
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
        PostTurnTask task;
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
            const std::string& conversation_memory_key =
                task.input.conversation_memory_key;
            const ConversationMemoryUpdate committed =
                task.response.updated_conversation_memory.value_or(
                    coordinator_->committedSnapshot(conversation_memory_key)
                        .value_or(ConversationMemoryUpdate{}));
            ConversationMemoryUpdate updated = orchestrator_->executePostTurn(
                task, committed, backend_);
            if (forceRequested()) {
                coordinator_->publish(
                    conversation_memory_key,
                    MemoryReadyResult::Status::Cancelled);
            } else {
                task.response.updated_conversation_memory = updated;
                coordinator_->publish(
                    conversation_memory_key,
                    MemoryReadyResult::Status::Success,
                    std::move(updated));
            }
        } catch (const std::exception& error) {
            LOG_WARN("[PostTurnWorker] Post-turn failed: model=" << model_id_
                     << " session=" << task.input.session_id
                     << " message=\"" << error.what() << "\"");
            coordinator_->publish(
                task.input.conversation_memory_key,
                forceRequested() ? MemoryReadyResult::Status::Cancelled
                                 : MemoryReadyResult::Status::Failed);
        } catch (...) {
            LOG_WARN("[PostTurnWorker] Post-turn failed: model=" << model_id_
                     << " session=" << task.input.session_id
                     << " message=<unknown>");
            coordinator_->publish(
                task.input.conversation_memory_key,
                forceRequested() ? MemoryReadyResult::Status::Cancelled
                                 : MemoryReadyResult::Status::Failed);
        }

        if (finish_post_turn_) {
            finish_post_turn_();
        }
    }
}

void PostTurnWorker::cancelTask(PostTurnTask& task) {
    coordinator_->publish(
        task.input.conversation_memory_key,
        MemoryReadyResult::Status::Cancelled);
    if (finish_post_turn_) {
        finish_post_turn_();
    }
}

bool PostTurnWorker::forceRequested() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return force_stop_;
}

} // namespace scheduler
