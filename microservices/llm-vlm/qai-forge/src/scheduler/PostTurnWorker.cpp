// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/scheduler/PostTurnWorker.h"

#include "qai_forge/backend/IGenerativeBackend.h"
#include "qai_forge/orchestration/IGenerativeOrchestrator.h"
#include "qai_forge/utils/Logger.h"

#include <optional>
#include <utility>

namespace scheduler {

namespace {

bool memoryChanged(const GenieMemoryState& before,
                   const GenieMemoryState& after) {
    return before.summary_content != after.summary_content ||
           before.summary_token_count != after.summary_token_count ||
           before.facts != after.facts ||
           before.canonical_boundary != after.canonical_boundary ||
           before.transcript_digest != after.transcript_digest;
}

ConversationMemoryUpdate legacyMemoryUpdate(const GenieMemoryState& memory) {
    ConversationMemoryUpdate update;
    update.summary_content = memory.summary_content;
    update.summary_token_count = memory.summary_token_count;
    update.facts = memory.facts;
    update.evicted_message_count = memory.canonical_boundary;
    return update;
}

} // namespace

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
            const GenieMemoryState& before = task.input.input_memory;
            GenieMemoryState updated = orchestrator_->executePostTurn(
                task, backend_);
            if (forceRequested()) {
                if (task.input.uses_private_memory &&
                    task.input.memory_turn.has_value()) {
                    coordinator_->abortTurn(
                        task.input.memory_turn.value(),
                        MemoryTurnState::Cancelled);
                } else {
                    coordinator_->publish(
                        conversation_memory_key,
                        MemoryReadyResult::Status::Cancelled);
                }
            } else if (task.input.uses_private_memory &&
                       task.input.memory_turn.has_value()) {
                const MemoryOutcome outcome = memoryChanged(before, updated)
                    ? MemoryOutcome::Updated
                    : MemoryOutcome::Unchanged;
                coordinator_->publish(
                    task.input.memory_turn.value(),
                    outcome,
                    outcome == MemoryOutcome::Updated
                        ? std::optional<GenieMemoryState>(std::move(updated))
                        : std::nullopt);
            } else {
                ConversationMemoryUpdate legacy = legacyMemoryUpdate(updated);
                task.response.updated_conversation_memory = legacy;
                coordinator_->publish(
                    conversation_memory_key,
                    MemoryReadyResult::Status::Success,
                    std::move(legacy));
            }
        } catch (const std::exception& error) {
            LOG_WARN("[PostTurnWorker] Post-turn failed: model=" << model_id_
                     << " session=" << task.input.session_id
                     << " message=\"" << error.what() << "\"");
            if (task.input.uses_private_memory &&
                task.input.memory_turn.has_value()) {
                if (forceRequested()) {
                    coordinator_->abortTurn(
                        task.input.memory_turn.value(),
                        MemoryTurnState::Cancelled);
                } else {
                    coordinator_->publish(
                        task.input.memory_turn.value(), MemoryOutcome::Failed);
                }
            } else {
                coordinator_->publish(
                    task.input.conversation_memory_key,
                    forceRequested() ? MemoryReadyResult::Status::Cancelled
                                     : MemoryReadyResult::Status::Failed);
            }
        } catch (...) {
            LOG_WARN("[PostTurnWorker] Post-turn failed: model=" << model_id_
                     << " session=" << task.input.session_id
                     << " message=<unknown>");
            if (task.input.uses_private_memory &&
                task.input.memory_turn.has_value()) {
                if (forceRequested()) {
                    coordinator_->abortTurn(
                        task.input.memory_turn.value(),
                        MemoryTurnState::Cancelled);
                } else {
                    coordinator_->publish(
                        task.input.memory_turn.value(), MemoryOutcome::Failed);
                }
            } else {
                coordinator_->publish(
                    task.input.conversation_memory_key,
                    forceRequested() ? MemoryReadyResult::Status::Cancelled
                                     : MemoryReadyResult::Status::Failed);
            }
        }

        if (finish_post_turn_) {
            finish_post_turn_();
        }
    }
}

void PostTurnWorker::cancelTask(PostTurnTask& task) {
    if (task.input.uses_private_memory &&
        task.input.memory_turn.has_value()) {
        coordinator_->abortTurn(
            task.input.memory_turn.value(), MemoryTurnState::Cancelled);
    } else {
        coordinator_->publish(
            task.input.conversation_memory_key,
            MemoryReadyResult::Status::Cancelled);
    }
    if (finish_post_turn_) {
        finish_post_turn_();
    }
}

bool PostTurnWorker::forceRequested() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return force_stop_;
}

} // namespace scheduler
