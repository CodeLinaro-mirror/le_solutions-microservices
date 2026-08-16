// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/scheduler/ConversationMemoryCoordinator.h"
#include "qai_forge/scheduler/GenerativeJob.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class IGenerativeBackend;
class IGenerativeOrchestrator;

namespace scheduler {

// Runs post-turn model work serially on the runtime's backend. It is never a
// scheduler job and is joined before backend teardown.
class PostTurnWorker {
public:
    PostTurnWorker(
        std::string model_id,
        IGenerativeBackend& backend,
        std::shared_ptr<IGenerativeOrchestrator> orchestrator,
        std::shared_ptr<ConversationMemoryCoordinator> coordinator,
        std::function<void()> finish_post_turn,
        size_t max_queue_depth = 1);
    ~PostTurnWorker();

    PostTurnWorker(const PostTurnWorker&) = delete;
    PostTurnWorker& operator=(const PostTurnWorker&) = delete;

    void start();
    bool enqueue(PostTurnTask task);
    void stop(bool force);

private:
    void workerLoop();
    void cancelTask(PostTurnTask& task);
    bool forceRequested() const;

    const std::string model_id_;
    IGenerativeBackend& backend_;
    std::shared_ptr<IGenerativeOrchestrator> orchestrator_;
    std::shared_ptr<ConversationMemoryCoordinator> coordinator_;
    std::function<void()> finish_post_turn_;
    const size_t max_queue_depth_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_thread_;
    std::deque<PostTurnTask> queue_;
    bool started_ = false;
    bool stop_requested_ = false;
    bool force_stop_ = false;
};

} // namespace scheduler
