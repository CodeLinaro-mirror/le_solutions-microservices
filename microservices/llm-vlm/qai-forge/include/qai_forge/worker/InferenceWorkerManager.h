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
#include <vector>
#include <cstdint>

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
//   F. Watchdog thread: A background thread monitors active inference and
//      kills the subprocess if it stops producing IPC activity. Idle worker
//      lifetime is owned exclusively by the model scheduler.
//      GENAI_WORKER_ACTIVE_TIMEOUT (default 600 s) applies while an EXECUTE
//      command is in flight.
//      The watchdog polls every 5 seconds. last_activity_time_ is updated on
//      every sendMessage() and readMessage() call so that any IPC traffic
//      resets the timer.
//   G. Prompt shared memory: the EXECUTE command's prompt text is not
//      inlined as a JSON string. Before fork()/exec(), startWorker() creates
//      a memfd-backed region (memfd_create + ftruncate + mmap, sized
//      GENAI_PROMPT_SHM_BYTES, default 4MB) and hands the fd/size to the
//      child via the PROMPT_SHM_FD/PROMPT_SHM_BYTES env vars — the same
//      inherited-fd convention already used for the socket fd. executeRequest()
//      memcpy's the prompt into this region via writePromptToShm() and sends
//      only a {"offset","len"} "prompt_ref" over the socket. The region is
//      fixed-size: a prompt that doesn't fit fails the request via on_error
//      rather than falling back to inline JSON.
//   H. Image shared memory (VLM only): mirrors Section G for preprocessed
//      VLM image tensors. Created only when process_type_ == "vlm", sized
//      GENAI_IMAGE_SHM_BYTES (default 32MB), exported via IMAGE_SHM_FD/
//      IMAGE_SHM_BYTES. VlmInferenceWorkerManager::executeVlmRequest() packs
//      each image into this region via writeImagesToShm() and sends an
//      "image_refs" array of {"offset","len"} instead of file paths.
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
    virtual ~InferenceWorkerManager();

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
    virtual void ensureWorkerRunning(const std::string& model_id,
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

    void executeStructuredRequest(const std::string& event_id,
                                  const json& messages,
                                  const json& tools,
                                  bool streaming,
                                  int max_tokens,
                                  float temperature,
                                  float top_p,
                                  int top_k,
                                  float presence_penalty,
                                  float frequency_penalty,
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
     * Initiate an eager background KV cache reset.
     *
     * Spawns a background thread that calls sendReset() (RESET + wait for READY).
     * Returns immediately — the reset runs concurrently with returning the
     * response to the HTTP layer and sending it to the client.
     *
     * The next executeRequest() call will wait for the reset to complete via
     * waitForPendingReset() before acquiring mutex_ and sending EXECUTE.
     * This ensures clean KV state with minimal added latency: by the time
     * the next request arrives, the reset is typically already done.
     *
     * Safe to call multiple times — joins any previous reset thread first.
     */
    void initiateBackgroundReset();

    /**
     * Wait for any pending background reset to complete.
     *
     * Called at the start of executeRequest() to ensure the KV cache is
     * clean before sending the EXECUTE command. If no reset is in progress,
     * returns immediately (no-op).
     */
    void waitForPendingReset();

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
     * Force-kill the active worker subprocess without taking the worker I/O mutex.
     * Returns true when SIGKILL was sent.
     */
    bool forceKillActiveWorker();

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

    // Bytes already read from sock_fd_ but not yet consumed as a full line —
    // carried across readMessage() calls so a single recv() can satisfy
    // multiple/partial lines without re-reading one byte at a time.
    std::string read_buf_;

    // memfd-backed shared-memory region for the EXECUTE command's prompt
    // text (Section G above). Single flat buffer, one direction (server
    // writes, worker reads) — unlike PredictiveWorkerManager's tensor region
    // there is no alignment requirement and no concurrent multi-tensor
    // packing, since only one prompt is in flight at a time under mutex_.
    // Created fresh per startWorker() call; torn down in cleanupWorker().
    void*  prompt_shm_ptr_   = nullptr;
    int    prompt_shm_fd_    = -1;
    size_t prompt_shm_bytes_ = 0;

    // memfd-backed shared-memory region for VLM image tensors (Section H
    // above). Only created when process_type_ == "vlm" — LLM/litert-lm
    // workers never touch this. Packs multiple images sequentially starting
    // at offset 0, same single-writer/single-reader reasoning as the prompt
    // region (only one EXECUTE in flight at a time under mutex_).
    // Created fresh per startWorker() call; torn down in cleanupWorker().
    void*  image_shm_ptr_   = nullptr;
    int    image_shm_fd_    = -1;
    size_t image_shm_bytes_ = 0;


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

    // Active silence is configurable; polling cadence is internal.
    int active_timeout_seconds_ = 600;  // GENAI_WORKER_ACTIVE_TIMEOUT
    int watchdog_interval_seconds_ = 5;

    // ── Eager background reset ─────────────────────────────────────────────
    // Background thread that runs sendReset() after each inference completes.
    // Initiated by initiateBackgroundReset(); joined by waitForPendingReset()
    // at the start of the next executeRequest() call.
    std::thread reset_thread_;

    // ── Internal helpers (private — not accessible to subclasses) ─────────────
    void startWorker(const std::string& model_id,
                     const std::string& config_file,
                     const std::string& sampler_config);
    void cleanupWorker(bool force = false);

    /**
     * Non-locking liveness check — caller must already hold mutex_.
     * kill(pid, 0) alone can't tell a live process apart from a zombie: an
     * exited-but-unreaped child still holds its PID, so kill() keeps
     * returning 0 until something waitpid()s it. This reaps the child if it
     * has exited so a crash is detected immediately instead of on the next
     * request's IPC write failure.
     */
    bool isWorkerRunningLocked() const;

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

    // True while an EXECUTE command is in flight. Managed by
    // sendExecuteAndStream() so every request path has identical watchdog
    // ownership and exception-safe cleanup.
    std::atomic<bool> is_active_{false};

    // ── Protected helpers for subclasses ──────────────────────────────────────

    /**
     * Send a pre-built EXECUTE command and stream responses back via callbacks.
     *
     * Caller MUST hold mutex_ before calling this method.
     * This method arms and disarms the active-inference watchdog state.
     * Called by executeRequest() and by VlmInferenceWorkerManager::executeVlmRequest()
     * so that the VLM subclass can inject extra fields (e.g. image_refs) into the
     * EXECUTE command before it is sent, without duplicating the streaming loop.
     */
    void sendExecuteAndStream(const json& execute_cmd,
                               const std::string& event_id,
                               TokenCallback on_token,
                               DoneCallback on_done,
                               ErrorCallback on_error);

    /**
     * Copy `prompt` into the prompt shared-memory region and return a
     * {"offset","len"} reference to pass as EXECUTE's "prompt_ref" field.
     *
     * Caller MUST hold mutex_ (only one EXECUTE is ever in flight, so the
     * region has no concurrent-write hazard and every prompt is written at
     * offset 0). Throws std::runtime_error if `prompt` exceeds
     * prompt_shm_bytes_ — callers should catch this and route it to
     * on_error rather than letting it propagate, since the region is
     * fixed-size with no inline-JSON fallback.
     */
    json writePromptToShm(const std::string& prompt);

    /**
     * Copy each image's bytes sequentially into the image shared-memory
     * region and return a JSON array of {"offset","len"} references, one
     * per image in input order, to pass as EXECUTE's "image_refs" field.
     *
     * Caller MUST hold mutex_ (only one EXECUTE is ever in flight, so the
     * region has no concurrent-write hazard). Only valid when
     * process_type_ == "vlm" (image_shm_ptr_ is null otherwise). Throws
     * std::runtime_error if the total size exceeds image_shm_bytes_ —
     * callers should catch this and route it to on_error rather than
     * letting it propagate, since the region is fixed-size with no
     * inline-JSON fallback.
     */
    json writeImagesToShm(const std::vector<std::vector<uint8_t>>& images);
};
