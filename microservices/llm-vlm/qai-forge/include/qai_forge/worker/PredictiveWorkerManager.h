// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/dto/TensorDTOs.h"
#include <string>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>

using json = nlohmann::ordered_json;

// ─────────────────────────────────────────────────────────────────────────────
// PredictiveWorkerManager — Layer 3 subprocess manager for Predictive AI
//
// Manages the lifecycle of a Predictive AI inference worker subprocess
// (qnn-inference-worker or snpe-inference-worker) and communicates with it
// via JSON Lines over a Unix domain socket.
//
// Protocol (same framing as GenIE workers, different message types):
//   Server → Worker: INIT, EXECUTE, SHUTDOWN
//   Worker → Server: READY, RESULT, ERROR
//
// The EXECUTE command carries base64-encoded input tensors.
// The RESULT response carries base64-encoded output tensors.
//
// Subprocess isolation: if the worker crashes (DSP fault, OOM), the socket
// closes, the error callback is invoked, and the next call to
// ensureWorkerRunning() starts a fresh worker.
//
// See docs/unified-inference-service.md §10 for the IPC protocol details.
// ─────────────────────────────────────────────────────────────────────────────

using PredictiveResultCallback = std::function<void(const TensorInferenceResponse&)>;
using PredictiveErrorCallback  = std::function<void(const std::string& message)>;

class PredictiveWorkerManager {
public:
    /**
     * @param worker_binary  Path to the worker binary
     *                       (e.g. /usr/local/bin/qnn-inference-worker)
     * @param process_type   Human-readable type for logging ("qnn", "snpe")
     */
    PredictiveWorkerManager(const std::string& worker_binary,
                               const std::string& process_type);
    ~PredictiveWorkerManager();

    // Non-copyable
    PredictiveWorkerManager(const PredictiveWorkerManager&) = delete;
    PredictiveWorkerManager& operator=(const PredictiveWorkerManager&) = delete;

    /**
     * Ensure the worker subprocess is running with the correct model loaded.
     * If the model has changed, the old worker is killed and a new one started.
     * If the worker has crashed, a new one is started automatically.
     *
     * @param init_params  JSON object with INIT command parameters.
     *                     For QNN:  {"model_file":..., "backend_lib":..., "sys_lib":...}
     *                     For SNPE: {"model_file":..., "delegate":..., "output_tensors":[...]}
     */
    void ensureWorkerRunning(const std::string& model_id,
                             const json&        init_params);

    /**
     * Execute a Predictive AI inference request.
     * Blocks until the RESULT or ERROR response is received.
     *
     * @param event_id   Unique ID for this inference event
     * @param request    Input tensors (raw bytes) + output names
     * @param on_result  Called with the output tensors on success
     * @param on_error   Called with an error message on failure
     */
    void executeInfer(const std::string&              event_id,
                      const TensorInferenceRequest&   request,
                      PredictiveResultCallback      on_result,
                      PredictiveErrorCallback       on_error);

    /**
     * Terminate the worker subprocess immediately (SIGKILL).
     */
    void terminateWorker(bool force = false);

    /**
     * Graceful shutdown — sends SHUTDOWN command, waits for exit.
     */
    void shutdown();

    bool isWorkerRunning() const;
    std::string getCurrentModelId() const;

private:
    std::string worker_binary_;
    std::string process_type_;
    std::string current_model_id_;
    std::string socket_path_;

    int worker_pid_ = -1;
    int sock_fd_    = -1;

    // Bytes already read from sock_fd_ but not yet consumed as a full line —
    // carried across readMessage() calls so a single recv() can satisfy
    // multiple/partial lines without re-reading one byte at a time.
    std::string read_buf_;

    void startWorker(const std::string& model_id, const json& init_params);
    void cleanupWorker(bool force = false);

    void sendMessage(const json& msg);
    json readMessage(int timeout_seconds = 30);

    static std::string generateSocketPath(const std::string& model_id,
                                          const std::string& process_type);
};
