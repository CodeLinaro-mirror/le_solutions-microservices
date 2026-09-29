// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// ThreadPool — minimal bounded, grow-only worker-thread pool for
// fire-and-forget task dispatch.
//
// Motivation: several qai-forge hot paths spawned a brand-new
// std::thread(...).detach() per unit of work — one thread per admitted
// scheduler job (ModelRuntime::executorLoop), and one thread per
// post-inference KV reset (InferenceWorkerManager::initiateBackgroundReset,
// which fires after *every* inference call under GenieOrchestrator's eager
// background-reset pattern). Each of those call sites is already bounded in
// *concurrency* by other logic upstream (max_concurrent_ admission gating,
// per-session reset dedup, etc.), so the unbounded OS thread creation on
// every call was pure overhead with no corresponding correctness
// requirement — the concurrency bound was already enforced elsewhere.
//
// This class provides a small pool of persistent worker threads that pull
// tasks off an internal queue instead of spawning a new thread per task.
// Submitting a task never blocks the caller and never spawns a new OS
// thread — the task is simply queued and picked up by whichever worker
// becomes free.
//
// Internal-only: consumed exclusively by scheduler/worker .cpp files, never
// exposed via include/qai_forge/.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace qai_forge {
namespace utils {

/**
 * A minimal fire-and-forget thread pool.
 *
 * - submit() never blocks and never spawns a new OS thread; the task is
 *   queued and executed by whichever persistent worker thread becomes free.
 * - resize() only grows the pool (never shrinks) — callers size the pool
 *   for the largest concurrency they expect (e.g. a backend's
 *   capabilities().max_concurrent, which is fixed once a model is loaded)
 *   and may call resize() again later if that value increases.
 * - shutdown() (also called from the destructor) signals every worker to
 *   exit once the queue drains, then joins them. Safe to call multiple
 *   times.
 * - An uncaught exception escaping a submitted task is swallowed rather
 *   than propagating out of the worker thread (which would otherwise call
 *   std::terminate() and take down the whole process) — this mirrors the
 *   fact that every caller of this pool already wraps its own submitted
 *   work in an internal try/catch, so this is purely a defensive backstop
 *   that also prevents one bad task from permanently shrinking pool
 *   capacity.
 */
class ThreadPool {
public:
    explicit ThreadPool(std::size_t thread_count) {
        resize(thread_count);
    }

    ~ThreadPool() {
        shutdown();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /**
     * Enqueue a task for execution by one of the pool's worker threads.
     * Never blocks the caller and never spawns a new OS thread.
     *
     * If the pool has already been shut down, the task is run inline on
     * the calling thread instead of being silently dropped — this avoids
     * losing fire-and-forget work (e.g. a KV reset) during a teardown
     * race where a caller submits just as the owning object's destructor
     * begins running.
     */
    void submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!stopped_) {
                tasks_.push(std::move(task));
                lock_notify_ = true;
            }
        }
        if (lock_notify_) {
            lock_notify_ = false;
            cv_.notify_one();
            return;
        }
        // Pool already shut down — run synchronously so the task still
        // completes rather than being dropped.
        task();
    }

    /**
     * Grow the pool to at least `thread_count` worker threads. No-op if
     * the pool already has at least that many threads, or if the pool has
     * already been shut down.
     */
    void resize(std::size_t thread_count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
            return;
        }
        while (workers_.size() < thread_count) {
            workers_.emplace_back(&ThreadPool::workerLoop, this);
        }
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return workers_.size();
    }

    /**
     * Signal every worker to exit once the task queue drains, then join
     * them. Safe to call multiple times — subsequent calls are a no-op.
     * Called automatically from the destructor.
     */
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_) {
                return;
            }
            stopped_ = true;
        }
        cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
    }

private:
    void workerLoop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] {
                    return stopped_ || !tasks_.empty();
                });
                if (tasks_.empty()) {
                    // stopped_ must be true here (the predicate above only
                    // wakes on stopped_ || !tasks_.empty()).
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            try {
                task();
            } catch (...) {
                // See class comment: swallow rather than std::terminate().
            }
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    bool stopped_ = false;
    // Scratch flag used only within submit() to decide, after releasing
    // mutex_, whether to notify a worker or fall back to running inline;
    // avoids calling cv_.notify_one() while still holding mutex_.
    bool lock_notify_ = false;
};

} // namespace utils
} // namespace qai_forge
