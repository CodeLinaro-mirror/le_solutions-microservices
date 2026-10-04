// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class BackendAdapter;
class PostTurnWork;

namespace scheduler {

// Runs post-turn work serially on the runtime's backend. It is never a
// scheduler job and is joined before backend teardown.
//
// This class is intentionally memory-agnostic — it only ever operates on
// the opaque PostTurnWork interface (defined alongside OrchestratorAdapter).
// It has no knowledge of conversation memory, MemoryTurnCommitToken, or
// ConversationMemoryCoordinator; all of that bookkeeping lives inside the
// concrete PostTurnWork implementation that OrchestratorAdapter constructs.
class PostTurnWorker {
public:
    PostTurnWorker(
        std::string model_id,
        BackendAdapter& backend,
        std::function<void(const std::string& session_id)> finish_post_turn,
        size_t max_queue_depth = 1);
    ~PostTurnWorker();

    PostTurnWorker(const PostTurnWorker&) = delete;
    PostTurnWorker& operator=(const PostTurnWorker&) = delete;

    void start();
    bool enqueue(std::unique_ptr<PostTurnWork> work);
    void setMaxQueueDepth(size_t max_queue_depth);
    void stop(bool force);

private:
    void workerLoop();
    bool forceRequested() const;

    const std::string model_id_;
    BackendAdapter& backend_;
    std::function<void(const std::string& session_id)> finish_post_turn_;
    size_t max_queue_depth_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_thread_;
    std::deque<std::unique_ptr<PostTurnWork>> queue_;
    bool started_ = false;
    bool stop_requested_ = false;
    bool force_stop_ = false;
};

} // namespace scheduler
