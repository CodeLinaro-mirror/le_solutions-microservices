// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// ConcurrencyMiddleware — P3 Implementation
//
// Enforces the DSP resource lock: only one session can execute inference
// at a time. Replaces the Python RequestQueueManager.
//
// Key design decisions (architecture_refactoring_design.md Section 3.D):
//   - Uses std::condition_variable for efficient blocking (no busy-wait).
//   - Same session can re-acquire without blocking (tool continuations).
//   - RAII Guard ensures the lock is always released, even on exceptions.
//   - Layer 2 (ChatOrchestratorImpl) is responsible for calling sendReset()
//     on the InferenceWorkerManager when a new session acquires the lock.
//     Layer 3 does NOT know about ADHOC_MODE — it just executes commands.
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/orchestration/ConcurrencyMiddleware.h"
#include "qai_forge/utils/Logger.h"

// ─────────────────────────────────────────────────────────────────────────────
// Singleton
// ─────────────────────────────────────────────────────────────────────────────
ConcurrencyMiddleware& ConcurrencyMiddleware::getInstance() {
    static ConcurrencyMiddleware instance;
    return instance;
}

// ─────────────────────────────────────────────────────────────────────────────
// acquire — block until the DSP lock is available for this session
// ─────────────────────────────────────────────────────────────────────────────
bool ConcurrencyMiddleware::acquire(const std::string& session_id, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);

    auto can_proceed = [this, &session_id]() {
        // Proceed if: no active session, OR same session (tool continuation)
        return active_session_id_.empty() || active_session_id_ == session_id;
    };

    if (timeout_ms > 0) {
        bool acquired = cv_.wait_for(lock,
            std::chrono::milliseconds(timeout_ms),
            can_proceed);
        if (!acquired) {
            LOG_WARN("[ConcurrencyMiddleware] Timeout waiting for DSP lock: " << session_id);
            return false;
        }
    } else {
        cv_.wait(lock, can_proceed);
    }

    active_session_id_ = session_id;
    LOG_DEBUG("[ConcurrencyMiddleware] DSP lock acquired by session: " << session_id);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// release — release the DSP lock and wake up waiting sessions
// ─────────────────────────────────────────────────────────────────────────────
void ConcurrencyMiddleware::release(const std::string& session_id) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (active_session_id_ == session_id) {
            active_session_id_.clear();
            LOG_DEBUG("[ConcurrencyMiddleware] DSP lock released by session: " << session_id);
        }
    }
    cv_.notify_all();
}

// ─────────────────────────────────────────────────────────────────────────────
// Accessors
// ─────────────────────────────────────────────────────────────────────────────
std::string ConcurrencyMiddleware::getActiveSessionId() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return active_session_id_;
}

bool ConcurrencyMiddleware::isActiveSession(const std::string& session_id) const {
    std::unique_lock<std::mutex> lock(mutex_);
    return active_session_id_ == session_id;
}

bool ConcurrencyMiddleware::isIdle() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return active_session_id_.empty();
}

// ─────────────────────────────────────────────────────────────────────────────
// RAII Guard
// ─────────────────────────────────────────────────────────────────────────────
ConcurrencyMiddleware::Guard::Guard(const std::string& session_id, int timeout_ms)
    : session_id_(session_id) {
    acquired_ = ConcurrencyMiddleware::getInstance().acquire(session_id, timeout_ms);
    if (!acquired_) {
        throw std::runtime_error(
            "ConcurrencyMiddleware: Timeout waiting for DSP lock for session: " + session_id
        );
    }
}

ConcurrencyMiddleware::Guard::~Guard() {
    if (acquired_) {
        ConcurrencyMiddleware::getInstance().release(session_id_);
    }
}
