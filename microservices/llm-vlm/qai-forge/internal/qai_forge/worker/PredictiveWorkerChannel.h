// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

using json = nlohmann::ordered_json;

// ─────────────────────────────────────────────────────────────────────────────
// PredictiveWorkerChannel — server-side multiplexed connection to ONE
// persistent Predictive AI worker subprocess that hosts MULTIPLE models.
//
// Replaces the older 1:1 "one subprocess per model" design with a 1:N
// "one subprocess per runtime type" channel. Every backend for the same
// runtime family (e.g. all QNN-backed
// predictive models) shares one instance of this class, which owns:
//   - the fork()/exec() lifecycle of the single worker subprocess;
//   - a dedicated reader thread that demultiplexes JSON-Lines responses by
//     "model_id" (LOAD_MODEL/UNLOAD_MODEL/READY/ERROR) or "event_id"
//     (EXECUTE/RESULT/ERROR), waking exactly one blocked caller thread;
//   - a write mutex serializing concurrent sendMessage() calls from multiple
//     model executor threads to prevent interleaved JSON lines on the wire.
//
// Shared-memory transport: each model keeps its OWN independent
// PredictiveSharedMemory region (see PredictiveSharedMemory.h), exactly as
// before. Because the worker process now outlives any single model and is
// not necessarily forked fresh for each new model's shm fd, a newly loaded
// model's shm fd is transferred into the already-running process via
// SCM_RIGHTS ancillary data over this channel's control socket
// (sendFdWithMessage()/ — the worker's LOAD_MODEL handler calls recvmsg()
// with a CMSG buffer to receive it). The first model loaded after the worker
// forks may still receive its fd via the traditional env-var + fork-time fd
// inheritance path for simplicity; all subsequent LOAD_MODEL calls MUST use
// fd-passing since the process already exists.
//
// Wire protocol additions (see also worker main.cpp files):
//   Server -> Worker  LOAD_MODEL   {"type":"LOAD_MODEL","model_id":...,
//                                   <backend-specific init fields>}
//                                   (shm fd delivered via SCM_RIGHTS
//                                   ancillary data on the same sendmsg())
//   Worker -> Server  READY        {"type":"READY","model_id":...,...}
//                  or  ERROR        {"type":"ERROR","model_id":...,"message":...}
//   Server -> Worker  UNLOAD_MODEL {"type":"UNLOAD_MODEL","model_id":...}
//   Worker -> Server  UNLOADED     {"type":"UNLOADED","model_id":...}
//   Server -> Worker  EXECUTE      {"type":"EXECUTE","event_id":...,
//                                   "model":...,...}                 (unchanged shape)
//   Worker -> Server  RESULT/ERROR {"type":"RESULT"|"ERROR","event_id":...,...}
//   Server -> Worker  SHUTDOWN     {"type":"SHUTDOWN"}               (unchanged)
//
// Fault isolation: if the worker process crashes, every model hosted in it
// becomes unhealthy simultaneously (this is the accepted trade-off versus the
// old 1-process-per-model design, in exchange for far lower steady-state
// resource cost). isAlive() detects the crash via POLLHUP on the control
// socket; the next loadModel() call for any affected model_id triggers a
// fresh spawn and LOAD_MODEL replay — callers do not need special-case logic
// for this, since PredictiveModelRuntime already retries ensureModelLoaded()
// on failure.
// ─────────────────────────────────────────────────────────────────────────────

using PredictiveChannelReadyValidator = std::function<void(const json& ready_message)>;

class PredictiveWorkerChannel {
public:
    /**
     * @param worker_binary  Path to the worker binary
     *                       (e.g. /usr/local/bin/qnn-inference-worker)
     * @param process_type   Human-readable type for logging ("qnn", "snpe", "litert")
     */
    PredictiveWorkerChannel(std::string worker_binary, std::string process_type);
    ~PredictiveWorkerChannel();

    PredictiveWorkerChannel(const PredictiveWorkerChannel&) = delete;
    PredictiveWorkerChannel& operator=(const PredictiveWorkerChannel&) = delete;

    /**
     * Ensure the shared worker process is running, then load (or reload)
     * one model into it. Blocks until READY or ERROR is received for this
     * model_id.
     *
     * @param model_id        Model identifier — the multiplexing key. Must be
     *                        globally unique across all backends sharing this
     *                        channel.
     * @param init_params     JSON object with LOAD_MODEL parameters (same
     *                        shape as the previous INIT payload for this
     *                        runtime family — model_file, backend-specific
     *                        fields, placement options, etc).
     * @param shm_fd          Backend-owned memfd for this model's dedicated
     *                        PredictiveSharedMemory region. Ownership is NOT
     *                        transferred — the caller keeps the fd open and
     *                        closes it in its own destructor as today; this
     *                        channel only borrows it long enough to pass a
     *                        duplicate descriptor to the worker.
     * @param shm_ptr          Local mapping of the SAME shm region (for
     *                        parity with the existing PredictiveSharedMemoryView
     *                        contract used elsewhere — this channel does not
     *                        touch it directly, only forwards dir_bytes to
     *                        the worker).
     * @param shm_dir_bytes   Per-direction byte capacity of the shm region.
     * @param ready_validator Optional callback validating the READY payload
     *                        before this call returns successfully. Throwing
     *                        from it fails the load.
     */
    void loadModel(const std::string& model_id,
                   const json& init_params,
                   int shm_fd,
                   void* shm_ptr,
                   size_t shm_dir_bytes,
                   PredictiveChannelReadyValidator ready_validator = {});

    /**
     * Unload one model from the shared worker process. No-op if the model
     * was never loaded or the worker isn't running. Does not affect other
     * models hosted in the same process.
     */
    void unloadModel(const std::string& model_id) noexcept;

    /**
     * Execute inference for one already-loaded model. Blocks until RESULT
     * or ERROR is received for this event_id. Thread-safe — multiple models'
     * executor threads may call this concurrently on the same channel.
     *
     * @param model_id   Which model's engine should run this request (also
     *                   present inside `request.model`, kept for symmetry
     *                   with the pre-multiplexing call sites).
     * @param event_id   Unique ID correlating this EXECUTE with its RESULT.
     * @param request    Compact worker IPC request (unchanged shape).
     * @param shm_dir_bytes Per-direction byte capacity, needed to bound-check
     *                   output data_ref offsets before copying out of shm.
     * @param shm_ptr    Local mapping of this model's shm region — outputs
     *                   are read directly out of it into the returned
     *                   TensorInferenceResponse.
     */
    struct ExecuteResult {
        bool ok = false;
        json outputs;         // raw "outputs" array from RESULT, or empty on error
        std::string error_message;
    };

    ExecuteResult executeInfer(const std::string& model_id,
                               const std::string& event_id,
                               const json& execute_message,
                               int timeout_seconds = 60);

    /** True if the worker subprocess is alive (best-effort, via POLLHUP). */
    bool isAlive() const noexcept;

    /** Force-terminate the shared worker process (SIGKILL) and reset state. */
    void terminate() noexcept;

    /** Graceful shutdown — sends SHUTDOWN, waits for exit. */
    void shutdown() noexcept;

private:
    struct PendingCall {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        json response;
    };

    void ensureProcessRunningLocked();
    void spawnProcessLocked();
    void cleanupProcessLocked(bool force) noexcept;
    void readerLoop();

    // Low-level framed I/O — a JSON line optionally preceded by an fd sent
    // via SCM_RIGHTS ancillary data (sendFd path only).
    void sendMessageLocked(const json& msg);
    void sendMessageWithFdLocked(const json& msg, int fd);
    // Reads exactly one '\n'-terminated JSON line from the socket. Used only
    // by the reader thread.
    bool readLineFromSocket(std::string& out_line);

    const std::string worker_binary_;
    const std::string process_type_;

    mutable std::mutex process_mutex_;   // guards pid_/sock_fd_/spawn lifecycle
    int worker_pid_ = -1;
    int sock_fd_ = -1;
    std::thread reader_thread_;
    std::atomic<bool> reader_running_{false};
    std::atomic<bool> shutting_down_{false};

    std::mutex write_mutex_;             // serializes sendMessage on sock_fd_

    // Correlation maps — guarded by calls_mutex_. model_id-keyed entries
    // service LOAD_MODEL/UNLOAD_MODEL replies (their READY/ERROR/UNLOADED
    // carries "model_id" but no "event_id"); event_id-keyed entries service
    // EXECUTE replies.
    std::mutex calls_mutex_;
    std::unordered_map<std::string, std::shared_ptr<PendingCall>> model_calls_;
    std::unordered_map<std::string, std::shared_ptr<PendingCall>> event_calls_;

    std::string read_buf_;  // reader-thread-local partial-line carryover
};
