// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <mutex>
#include <condition_variable>
#include <string>
#include <optional>
#include <chrono>
#include <iostream>

// ─────────────────────────────────────────────────────────────────────────────
// ConcurrencyMiddleware — P3 (Section 3.D of architecture design)
//
// Replaces the Python RequestQueueManager. Enforces the core invariant:
//   Only ONE session can have an active inference request at any time.
//
// Design decisions:
//   - Uses std::mutex + std::condition_variable for blocking acquisition.
//   - The active_session_id tracks which session currently holds the DSP lock.
//   - Tool continuations (same session) can proceed without waiting.
//   - Layer 3 (InferenceWorkerManager) is called ONLY after the lock is acquired.
//   - Layer 2 calls sendReset() on the InferenceWorkerManager when a different
//     session acquires the lock (ADHOC mode context clearing).
//
// This is a singleton — one DSP resource lock per server process.
// ─────────────────────────────────────────────────────────────────────────────
class ConcurrencyMiddleware {
public:
    static ConcurrencyMiddleware& getInstance();

    // ── Lock acquisition ───────────────────────────────────────────────────────

    /**
     * Acquire the DSP lock for the given session.
     * Blocks until the lock is available (no other session is active).
     * If the same session already holds the lock (tool continuation), returns immediately.
     *
     * @param session_id  The session requesting the lock
     * @param timeout_ms  Max wait time in milliseconds (0 = infinite)
     * @return            true if lock acquired, false if timeout expired
     */
    bool acquire(const std::string& session_id, int timeout_ms = 0);

    /**
     * Release the DSP lock for the given session.
     * Wakes up any waiting sessions.
     */
    void release(const std::string& session_id);

    /**
     * Returns the currently active session ID, or empty string if idle.
     */
    std::string getActiveSessionId() const;

    /**
     * Returns true if the given session currently holds the lock.
     */
    bool isActiveSession(const std::string& session_id) const;

    /**
     * Returns true if no session currently holds the lock.
     */
    bool isIdle() const;

    // ── RAII guard ─────────────────────────────────────────────────────────────

    /**
     * RAII guard that acquires the lock on construction and releases on destruction.
     * Usage:
     *   {
     *     ConcurrencyMiddleware::Guard guard(session_id);
     *     // ... inference ...
     *   } // lock released automatically
     */
    class Guard {
    public:
        Guard(const std::string& session_id, int timeout_ms = 300000);
        ~Guard();
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        bool acquired() const { return acquired_; }
    private:
        std::string session_id_;
        bool acquired_ = false;
    };

private:
    ConcurrencyMiddleware() = default;
    ConcurrencyMiddleware(const ConcurrencyMiddleware&) = delete;
    ConcurrencyMiddleware& operator=(const ConcurrencyMiddleware&) = delete;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::string active_session_id_;
};
