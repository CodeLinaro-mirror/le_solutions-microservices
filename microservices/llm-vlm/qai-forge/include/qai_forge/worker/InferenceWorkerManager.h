// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/worker/InferenceProtocol.h"
#include "qai_forge/InternalDTOs.h"
#include <string>
#include <functional>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <condition_variable>

// ─────────────────────────────────────────────────────────────────────────────
// InferenceWorkerManager — Layer 3 (Process Management & IPC)
//
// Manages the lifecycle of the genai-inference-worker subprocess and
// communicates with it via JSON Lines over a Unix Domain Socket (socketpair).
//
// Design decisions:
//   A. Synchronous socket I/O: readMessage() uses select() for timeout.
//      Concurrency is handled at Layer 1 (Drogon thread pool) and Layer 2
//      (ConcurrencyMiddleware DSP lock). No threading or queues in Layer 3.
//   B. Typed IPC Events: Yields IPCTokenEvent, IPCDoneEvent, IPCErrorEvent
//      instead of raw dictionary parsing.
//   C. bypass_think_filter: Passes raw <think> tokens to Layer 2's
//      ReasoningRouter when the model supports thinking.
//   D. No ADHOC_MODE logic: Layer 2 (ConcurrencyMiddleware) decides when
//      to call sendReset(). Layer 3 just executes commands blindly.
//   E. Unique socket IDs: Socket path includes a UUID to prevent clashes
//      when multiple workers run for the same model.
//   F. Watchdog thread: A background thread monitors worker activity and
//      kills the subprocess if it becomes unresponsive. Two configurable
//      thresholds apply:
//        - Idle timeout  (GENAI_WORKER_IDLE_TIMEOUT,   default 60 s):
//          applies when no EXECUTE is in flight.
//        - Active timeout (GENAI_WORKER_ACTIVE_TIMEOUT, default 600 s):
//          applies while an EXECUTE command is in flight (long inference).
//      The watchdog polls every GENAI_WATCHDOG_CHECK_INTERVAL seconds
//      (default 5 s). last_activity_time_ is updated on every sendMessage()
//      and readMessage() call so that any IPC traffic resets the timer.
//
// Subprocess architecture:
//   - Worker subprocess links libllmengine.so (LLM) or libvlmengine.so (VLM).
//   - Worker crashes are detected via socket close (EOF) → server returns error.
//   - Cancellation is via SIGKILL to the worker PID.
//   - Child processes inherit dropped privileges from the parent server.
// ─────────────────────────────────────────────────────────────────────────────

// Callback types for streaming token delivery to Layer 2
using TokenCallback  = std::function<void(const IPCTokenEvent&)>;
using DoneCallback   = std::function<void(const IPCDoneEvent&)>;
using ErrorCallback  = std::function<void(const IPCErrorEvent&)>;

class InferenceWorkerManager {
public:
    /**
     * Constructor.
     * @param process_type  "llm" or "vlm" — used for logging and socket naming.
     */
    explicit InferenceWorkerManager(const std::string& process_type);
    ~InferenceWorkerManager();

    // ── Worker lifecycle ───────────────────────────────────────────────────────

    /**
     * Ensure the worker subprocess is running with the specified model.
     * If the model has changed, the old worker is terminated and a new one started.
     * Called by Layer 2 before each inference request.
     *
     * @param model_id          Model identifier
     * @param config_file       Absolute path to the processed genie_config.json
     * @param sampler_config    Absolute path to the sampler config
     */
    void ensureWorkerRunning(const std::string& model_id,
                              const std::string& config_file,
                              const std::string& sampler_config);

    /**
     * Execute an inference request and stream tokens back to Layer 2.
     *
     * Layer 2 provides the callbacks; this method calls them synchronously
     * as tokens arrive from the worker subprocess.
     *
     * @param event_id          Unique ID for this inference event
     * @param prompt            The compacted context prompt (from Layer 2)
     * @param streaming         Whether to stream tokens or accumulate
     * @param max_tokens        Max completion tokens
     * @param temperature       Sampling temperature
     * @param top_p             Top-p sampling
     * @param top_k             Top-k sampling
     * @param presence_penalty  Presence penalty
     * @param frequency_penalty Frequency penalty
     * @param bypass_think_filter  If true, raw <think> tokens pass to Layer 2
     * @param on_token          Called for each TOKEN event
     * @param on_done           Called when DONE event received
     * @param on_error          Called on ERROR event or socket failure
     */
    void executeRequest(const std::string& event_id,
                        const std::string& prompt,
                        bool streaming,
                        int max_tokens,
                        float temperature,
                        float top_p,
                        int top_k,
                        float presence_penalty,
                        float frequency_penalty,
                        bool bypass_think_filter,
                        TokenCallback on_token,
                        DoneCallback on_done,
                        ErrorCallback on_error);

    /**
     * Send a RESET command and wait for the matching READY response.
     * Called by Layer 2's ConcurrencyMiddleware (not by Layer 3 itself).
     * This is the key design change from the Python implementation where
     * ADHOC_MODE logic was embedded in Layer 3.
     */
    void sendReset();

    /**
     * Save the KV cache to a named checkpoint.
     * Used by Layer 2 for reasoning rewind (Section 6.B) and
     * ADHOC mode context swapping (Section 6.C).
     */
    bool saveKvCache(const std::string& checkpoint_name);

    /**
     * Restore the KV cache from a named checkpoint.
     */
    bool restoreKvCache(const std::string& checkpoint_name);

    /**
     * Terminate the worker subprocess immediately (SIGKILL).
     * Called by Layer 2's cancelSession() to abort active inference.
     * Child processes inherit dropped privileges — no privilege escalation needed.
     */
    void terminateWorker(bool force = false);

    /**
     * Gracefully shut down the worker (sends SHUTDOWN command, then waits).
     */
    void shutdown();

    bool isWorkerRunning() const;
    std::string getCurrentModelId() const;

    /**
     * Return the timestamp of the last IPC activity (send or receive).
     * Exposed for testing and diagnostics.
     */
    std::chrono::steady_clock::time_point lastActivityTime() const;

private:
    std::string process_type_;
    std::string current_model_id_;
    std::string socket_path_;

    // Subprocess handle — raw pid_t for portability without Boost in header
    int worker_pid_ = -1;
    int sock_fd_ = -1;

    // ── Watchdog ───────────────────────────────────────────────────────────
    // Background thread that kills the worker if it stops producing IPC
    // traffic within the configured timeout window.
    //
    // Thread-safety contract:
    //   - watchdog_thread_ is started/stopped only from startWatchdog() /
    //     stopWatchdog(), which are called while mutex_ is held by the caller
    //     of cleanupWorker() / startWorker().
    //   - The watchdog thread itself NEVER acquires mutex_. It reads only
    //     atomic members (watchdog_target_pid_, last_activity_time_,
    //     is_active_, watchdog_stop_) and sends SIGKILL directly.
    //     This prevents a deadlock where cleanupWorker() (holding mutex_)
    //     joins the watchdog thread while the watchdog thread waits for mutex_.
    std::thread watchdog_thread_;
    std::condition_variable watchdog_cv_;
    std::mutex watchdog_cv_mutex_;
    std::atomic<bool> watchdog_stop_{false};

    // The PID the watchdog is currently monitoring.
    // Set to the worker PID after a successful start; cleared to -1 before
    // SIGKILL is sent (prevents double-kill) and in cleanupWorker().
    // Written under mutex_; read atomically by the watchdog thread.
    std::atomic<int> watchdog_target_pid_{-1};

    // Timestamp of the last successful sendMessage() or readMessage() call.
    // Updated atomically; the watchdog reads it without holding mutex_.
    std::atomic<std::chrono::steady_clock::time_point> last_activity_time_{
        std::chrono::steady_clock::time_point{}};

    // Configurable timeouts (read once from env in constructor).
    int idle_timeout_seconds_   = 60;   // GENAI_WORKER_IDLE_TIMEOUT
    int active_timeout_seconds_ = 600;  // GENAI_WORKER_ACTIVE_TIMEOUT
    int watchdog_interval_seconds_ = 5; // GENAI_WATCHDOG_CHECK_INTERVAL

    // ── Internal helpers (private — not accessible to subclasses) ─────────────
    void startWorker(const std::string& model_id,
                     const std::string& config_file,
                     const std::string& sampler_config);
    void cleanupWorker(bool force = false);

    /**
     * Entry point for the watchdog background thread.
     * Periodically checks last_activity_time_ against the configured
     * timeout and calls terminateWorker(true) if the worker is unresponsive.
     */
    void watchdogThreadFunc();

    /**
     * Start the watchdog thread (called after a worker is successfully started).
     * Safe to call multiple times — stops any existing watchdog first.
     */
    void startWatchdog();

    /**
     * Stop and join the watchdog thread (called from cleanupWorker).
     * Safe to call while mutex_ is held because the watchdog thread
     * never acquires mutex_ — it only reads atomics and sends SIGKILL.
     */
    void stopWatchdog();

    void sendMessage(const json& msg);
    json readMessage(int timeout_seconds = 30);

    /**
     * Send a command and wait for a READY response with matching command_id.
     * Drains any stale TOKEN/DONE messages while waiting.
     */
    bool sendCommandAndWaitReady(const json& cmd, int timeout_seconds = 30);

    /**
     * Generate a unique socket path: /tmp/genai-worker-{model_id}-{uuid}.sock
     * Section 4.E: UUID suffix prevents clashes when multiple workers run
     * for the same model.
     */
    static std::string generateSocketPath(const std::string& model_id);

protected:
    // ── Protected state — accessible to subclasses (e.g. VlmInferenceWorkerManager)

    // Mutex guards all socket I/O and worker lifecycle operations.
    // VlmInferenceWorkerManager::executeVlmRequest() must hold this before
    // calling sendExecuteAndStream().
    mutable std::mutex mutex_;

    // True while an EXECUTE command is in flight.
    std::atomic<bool> is_active_{false};

    // ── Protected helpers for subclasses ──────────────────────────────────────

    /**
     * Send a pre-built EXECUTE command and stream responses back via callbacks.
     *
     * Caller MUST hold mutex_ before calling this method.
     * Called by executeRequest() and by VlmInferenceWorkerManager::executeVlmRequest()
     * so that the VLM subclass can inject extra fields (e.g. image_urls) into the
     * EXECUTE command before it is sent, without duplicating the streaming loop.
     */
    void sendExecuteAndStream(const json& execute_cmd,
                               const std::string& event_id,
                               TokenCallback on_token,
                               DoneCallback on_done,
                               ErrorCallback on_error);
};
