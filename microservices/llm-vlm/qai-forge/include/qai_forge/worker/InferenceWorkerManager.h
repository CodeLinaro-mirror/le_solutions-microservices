// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/worker/InferenceProtocol.h"
#include "qai_forge/InternalDTOs.h"
#include <string>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <thread>
#include <future>
#include <chrono>
#include <condition_variable>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <exception>

// ─────────────────────────────────────────────────────────────────────────────
// InferenceWorkerManager — Layer 3 (Process Management & IPC)
//
// Manages the lifecycle of the genai-inference-worker subprocess and
// communicates with it via JSON Lines over a Unix Domain Socket (socketpair).
//
// Design decisions:
//   A. Concurrent requests: a dedicated background reader thread continuously
//      drains the socket and demuxes responses by event_id (EXECUTE stream:
//      TOKEN/DONE/ERROR) or command_id (RESET/SAVE_KV/RESTORE_KV/ABORT:
//      READY/ERROR) to whichever caller thread is waiting on that id via a
//      std::promise/std::future pair. Caller threads never read the socket
//      directly. This allows up to max_slots_ EXECUTE requests to be
//      in flight at once (continuous batching) while non-CB models
//      (max_slots_ == 1) see identical serialized behavior to before.
//   B. Typed IPC Events: Yields IPCTokenEvent, IPCDoneEvent, IPCErrorEvent
//      instead of raw dictionary parsing.
//   C. bypass_think_filter: Passes raw <think> tokens to Layer 2's
//      ReasoningRouter when the model supports thinking.
//   D. No ADHOC_MODE logic: Layer 2 (ConcurrencyMiddleware) decides when
//      to call sendReset(). Layer 3 just executes commands blindly.
//   E. Unique socket IDs: Socket path includes a UUID to prevent clashes
//      when multiple workers run for the same model.
//   F. Watchdog thread: A background thread monitors active inference and
//      kills the subprocess if it stops producing IPC activity while
//      active_request_count_ > 0. Idle worker lifetime is owned exclusively
//      by the model scheduler. GENAI_WORKER_ACTIVE_TIMEOUT (default 600s)
//      applies while at least one EXECUTE command is in flight.
//   G. Prompt shared memory (VLM only): startWorker() creates one fixed-size
//      memfd-backed region and hands its fd/size to the VLM child via
//      PROMPT_SHM_FD/PROMPT_SHM_BYTES. Requests use offset zero through
//      writePromptToShm(). LLM prompts are sent inline.
//   H. Image shared memory (VLM only): startWorker() creates one fixed-size
//      region for preprocessed VLM image tensors and exports it via
//      IMAGE_SHM_FD/IMAGE_SHM_BYTES. VLM is not continuous-batched, so no
//      shared-memory slot pool is needed.
//
// Subprocess architecture:
//   - Worker subprocess links libllmengine.so (LLM) or libvlmengine.so (VLM).
//   - Worker crashes are detected via socket close (EOF) → server returns
//     error to every pending request/command and stops the reader thread.
//   - Whole-worker cancellation is via SIGKILL to the worker PID; per-session
//     cancellation under continuous batching uses sendAbort(session_id).
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
     * @param process_type  "llm", "vlm", or "litert-lm" — used for logging,
     *                       socket naming, and max-slot derivation.
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

    // Set the effective concurrency selected by the backend before startup.
    void setMaxSlots(int max_slots);

    /**
     * Execute an inference request and stream tokens back to Layer 2.
     *
     * Layer 2 provides the callbacks; this method calls them (from the
     * background reader thread) as tokens arrive from the worker subprocess.
     * Multiple concurrent calls are supported when the loaded model has
     * max_slots_ > 1 (continuous batching); otherwise calls are effectively
     * serialized by the single shm slot.
     *
     * @param event_id          Unique ID for this inference event
     * @param session_id        Continuous-batching slot/routing key. Pass ""
     *                          for single-slot models (default behavior).
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
                        const std::string& session_id,
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
                        ErrorCallback on_error,
                        bool kv_invalidated = false);

    void executeStructuredRequest(const std::string& event_id,
                                  const std::string& session_id,
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
     *
     * @param session_id  Session whose KV slot to reset. "" resets the
     *                    worker's legacy single-slot engine.
     */
    void sendReset(const std::string& session_id = "");

    /**
     * Initiate an eager background KV cache reset for one session.
     *
     * Spawns a background thread that calls sendReset(session_id) (RESET +
     * wait for READY). Returns immediately — the reset runs concurrently
     * with returning the response to the HTTP layer and sending it to the
     * client.
     *
     * Callers wait for the shared reset completion via
     * waitForPendingReset(session_id) before sending EXECUTE.
     *
     * Safe to call multiple times for the same session_id; any previous
     * reset for that session is joined before a new one is started.
     */
    void initiateBackgroundReset(const std::string& session_id);

    /**
     * Wait for the shared pending background reset for the given session to
     * complete. Multiple callers may wait for the same reset.
     *
     * Called at the start of executeRequest()/executeStructuredRequest() so
     * the KV cache is clean before sending EXECUTE. If no reset is pending
     * for this session, returns immediately (no-op).
     */
    void waitForPendingReset(const std::string& session_id);
    void waitForSessionReady(const std::string& session_id);

    /**
     * Save the KV cache to a named checkpoint.
     * Used by Layer 2 for reasoning rewind (Section 6.B) and
     * ADHOC mode context swapping (Section 6.C).
     */
    bool saveKvCache(const std::string& checkpoint_name,
                      const std::string& session_id = "");

    /**
     * Restore the KV cache from a named checkpoint.
     */
    bool restoreKvCache(const std::string& checkpoint_name,
                         const std::string& session_id = "");

    /**
     * Send a session-scoped ABORT command. Stops that session's in-flight
     * generation without affecting other concurrent sessions' engines.
     * Returns false if the worker isn't running, the command times out, or
     * the worker reports an error — callers should fall back to
     * forceKillActiveWorker() in that case.
     */
    bool sendAbort(const std::string& session_id);

    /**
     * Send a CLEAR_SESSION command to release per-session KV state in the worker.
     * Used by LiteRT-LM backend to free g_kv_sessions entries after a session ends.
     */
    void sendClearSession(const std::string& session_id);

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
     * Continuous-batching slot capacity for the currently loaded model.
     * Always 1 for VLM/LiteRT-LM process types and for LLM models without
     * a batching config.
     */
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
    // owned exclusively by the reader thread once it starts.
    std::string read_buf_;

    // Fixed-size VLM-only prompt shared-memory region. LLM prompts are inline.
    void*  prompt_shm_ptr_        = nullptr;
    int    prompt_shm_fd_         = -1;
    size_t prompt_shm_bytes_      = 0;  // fixed VLM mapping size

    // memfd-backed shared-memory region for VLM image tensors (Section H
    // above). Only created when process_type_ == "vlm" — LLM/litert-lm
    // workers never touch this. Created fresh per startWorker() call;
    // torn down in cleanupWorker().
    void*  image_shm_ptr_        = nullptr;
    int    image_shm_fd_         = -1;
    size_t image_shm_bytes_      = 0;  // fixed VLM mapping size

    // ── Continuous-batching slot capacity ──────────────────────────────────
    // Supplied by the backend before worker startup
    // The backend supplies the effective value; VLM and LiteRT-LM are forced to 1. Governs
    // LLM max_slots_ controls session concurrency; VLM shared memory is single-slot.
    int max_slots_ = 1;

    // ── Watchdog ───────────────────────────────────────────────────────────
    // Background thread that kills the worker if it stops producing IPC
    // traffic within the configured timeout window while requests are active.
    //
    // Thread-safety contract:
    //   - watchdog_thread_ is started/stopped only from startWatchdog() /
    //     stopWatchdog(), which are called while the lifecycle lock is held
    //     by the caller of cleanupWorker() / startWorker().
    //   - The watchdog thread itself NEVER acquires mutex_. It reads only
    //     atomic members (watchdog_target_pid_, last_activity_time_,
    //     active_request_count_, watchdog_stop_) and sends SIGKILL directly.
    //     This prevents a deadlock where cleanupWorker() (holding the
    //     exclusive lifecycle lock) joins the watchdog thread while the
    //     watchdog thread waits for that lock.
    std::thread watchdog_thread_;
    std::condition_variable watchdog_cv_;
    std::mutex watchdog_cv_mutex_;
    std::atomic<bool> watchdog_stop_{false};

    // The PID the watchdog is currently monitoring.
    // Set to the worker PID after a successful start; cleared to -1 before
    // SIGKILL is sent (prevents double-kill) and in cleanupWorker().
    std::atomic<int> watchdog_target_pid_{-1};

    // Timestamp of the last successful sendMessage() or reader-thread
    // message receipt. Updated atomically; the watchdog reads it without
    // holding any lock.
    std::atomic<std::chrono::steady_clock::time_point> last_activity_time_{
        std::chrono::steady_clock::time_point{}};

    // Active silence is configurable; polling cadence is internal.
    int active_timeout_seconds_ = 600;  // GENAI_WORKER_ACTIVE_TIMEOUT
    int watchdog_interval_seconds_ = 5;

    // Number of EXECUTE requests currently in flight. Replaces a single
    // is_active_ bool now that multiple requests can overlap under
    // continuous batching. The watchdog treats > 0 as "active".
    std::atomic<int> active_request_count_{0};

    // ── Eager background reset (per session) ───────────────────────────────
    // A reset can have multiple waiters (the runtime bookkeeping path and the
    // next inference/post-turn path). Keep completion shared so one waiter
    // cannot consume the reset before the others observe it.
    struct PendingReset {
        std::promise<void> completion_promise;
        std::shared_future<void> completion;
        std::mutex worker_mutex;
        std::thread worker;

        PendingReset()
            : completion(completion_promise.get_future().share()) {}
    };

    std::mutex reset_threads_mutex_;
    std::unordered_map<std::string, std::shared_ptr<PendingReset>> reset_threads_;
    void joinAllResetThreads();
    void sendReinitialize(const std::string& session_id);

    // ── Reader-thread demux (Section A) ─────────────────────────────────────
    struct PendingExecute {
        TokenCallback on_token;
        DoneCallback  on_done;
        ErrorCallback on_error;
        std::promise<void> done_promise;
    };
    struct PendingCommand {
        std::promise<bool> promise;
        std::string error_message;
    };

    std::mutex pending_execute_mutex_;
    std::unordered_map<std::string, std::shared_ptr<PendingExecute>> pending_execute_;

    std::mutex pending_command_mutex_;
    std::unordered_map<std::string, std::shared_ptr<PendingCommand>> pending_command_;

    std::thread reader_thread_;
    std::atomic<bool> reader_stop_{false};

    void readerThreadFunc();
    void dispatchExecuteEvent(const std::string& type, const std::string& event_id, const json& msg);
    void dispatchCommandEvent(const std::string& type, const std::string& command_id, const json& msg);
    void failAllPending(const std::string& reason);

    // Serializes sendMessage() calls onto sock_fd_ across concurrent callers.
    std::mutex write_mutex_;

    // ── Internal helpers (private — not accessible to subclasses) ─────────────
    void startWorker(const std::string& model_id,
                     const std::string& config_file,
                     const std::string& sampler_config);
    void cleanupWorker(bool force = false);

    /**
     * Non-locking liveness check — caller must already hold the exclusive
     * lifecycle lock.
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
     * timeout (while active_request_count_ > 0) and SIGKILLs the worker if
     * unresponsive.
     */
    void watchdogThreadFunc();

    /**
     * Start the watchdog thread (called after a worker is successfully started).
     * Safe to call multiple times — stops any existing watchdog first.
     */
    void startWatchdog();

    /**
     * Stop and join the watchdog thread (called from cleanupWorker).
     */
    void stopWatchdog();

    // sendMessage() takes write_mutex_ internally — safe to call from any thread.
    void sendMessage(const json& msg);

    // readMessage() must only ever be called from the reader thread (or,
    // before the reader thread is started, synchronously during the
    // startWorker() handshake).
    json readMessage(int timeout_seconds = 30);

    /**
     * Send a command, register it in pending_command_, and block on its
     * future until READY/ERROR arrives (matched by command_id) or the
     * timeout elapses. On failure, populates PendingCommand::error_message
     * accessible to the caller via the returned bool (false).
     */
    bool sendCommandAndWait(const json& cmd, const std::string& command_id, int timeout_seconds = 30);

    /**
     * Generate a short random command id with the given prefix, e.g.
     * "reset-1a2b3c4d".
     */
    static std::string generateCommandId(const std::string& prefix);

    /**
     * Generate a unique socket path: /tmp/genai-worker-{model_id}-{uuid}.sock
     * Section 4.E: UUID suffix prevents clashes when multiple workers run
     * for the same model.
     */
    static std::string generateSocketPath(const std::string& model_id);

protected:
    // ── Protected state — accessible to subclasses (e.g. VlmInferenceWorkerManager)

    // Exclusive lock: worker lifecycle (start/cleanup/model-switch/shutdown).
    // Shared lock: per-request operations (executeRequest, sendReset,
    // saveKvCache, restoreKvCache, sendAbort) — held for the duration of the
    // request so lifecycle changes can't tear down sock_fd_/shm regions out
    // from under an in-flight request, while still allowing N concurrent
    // shared holders when max_slots_ > 1.
    mutable std::shared_mutex mutex_;

    // ── Protected helpers for subclasses ──────────────────────────────────────

    /**
     * Send a pre-built EXECUTE command and stream responses back via
     * callbacks (invoked from the reader thread). Blocks the calling thread
     * until DONE/ERROR arrives or the request times out.
     *
     * Caller MUST hold a shared lock on mutex_ before calling this method,
     * and MUST have already claimed a shm slot for the command's
     * prompt_ref/image_refs offsets. Extracts session_id from execute_cmd
     * (defaults to "") for error attribution.
     *
     * Called by executeRequest() and by
     * VlmInferenceWorkerManager::executeVlmRequest() so that the VLM
     * subclass can inject extra fields (e.g. image_refs) into the EXECUTE
     * command before it is sent, without duplicating the demux/wait logic.
     */
    void sendExecuteAndStream(const json& execute_cmd,
                               const std::string& event_id,
                               TokenCallback on_token,
                               DoneCallback on_done,
                               ErrorCallback on_error);

    /**
     * Copy `prompt` into the prompt shared-memory region at the slot given
     * by `offset 0` and return a {"offset","len"} reference to pass as
     * EXECUTE's "prompt_ref" field.
     *
     * Caller MUST hold a shared lock on mutex_ and have acquired
     * a single fixed offset-zero region. Throws std::runtime_error if `prompt` exceeds
     * prompt_shm_bytes_ — callers should catch this and route it to
     * on_error rather than letting it propagate, since the region is
     * fixed-size with no inline-JSON fallback.
     */
    json writePromptToShm(const std::string& prompt);

    /**
     * Copy each image's bytes sequentially into the image shared-memory
     * region at fixed offset zero, and return a JSON
     * array of {"offset","len"} references, one per image in input order,
     * to pass as EXECUTE's "image_refs" field.
     *
     * Caller MUST hold a shared lock on mutex_. Only valid when
     * process_type_ == "vlm" (image_shm_ptr_ is null otherwise), which
     * always has max_slots_ == 1, so offset 0 is always 0 in practice.
     * Throws std::runtime_error if the total size exceeds
     * image_shm_bytes_ — callers should catch this and route it to
     * on_error rather than letting it propagate.
     */
    json writeImagesToShm(const std::vector<std::vector<uint8_t>>& images);

};
