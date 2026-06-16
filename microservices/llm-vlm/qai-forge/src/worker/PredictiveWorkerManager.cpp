// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// PredictiveWorkerManager — Layer 3 subprocess manager for Predictive AI
//
// Manages qnn-inference-worker and snpe-inference-worker subprocesses.
// Uses the same fork/exec + Unix socket IPC pattern as InferenceWorkerManager,
// with a different message protocol (EXECUTE/RESULT instead of TOKEN/DONE).
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/worker/PredictiveWorkerManager.h"
#include "qai_forge/utils/Logger.h"

#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>

#include <sstream>
#include <stdexcept>
#include <cstring>
#include <random>
#include <iomanip>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// Base64 encode/decode (no external dependency)
// ─────────────────────────────────────────────────────────────────────────────

static const char B64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64Encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = (uint32_t)data[i] << 16;
        if (i + 1 < len) b |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) b |= (uint32_t)data[i + 2];
        out += B64_CHARS[(b >> 18) & 0x3F];
        out += B64_CHARS[(b >> 12) & 0x3F];
        out += (i + 1 < len) ? B64_CHARS[(b >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? B64_CHARS[b & 0x3F]        : '=';
    }
    return out;
}

static std::vector<uint8_t> base64Decode(const std::string& s) {
    static const int8_t LUT[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    std::vector<uint8_t> out;
    out.reserve((s.size() / 4) * 3);
    uint32_t b = 0;
    int bits = 0;
    for (char c : s) {
        if (c == '=') break;
        int8_t v = LUT[(uint8_t)c];
        if (v < 0) continue;
        b = (b << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)((b >> bits) & 0xFF));
        }
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// PredictiveWorkerManager
// ─────────────────────────────────────────────────────────────────────────────

PredictiveWorkerManager::PredictiveWorkerManager(
    const std::string& worker_binary,
    const std::string& process_type)
    : worker_binary_(worker_binary)
    , process_type_(process_type)
{}

PredictiveWorkerManager::~PredictiveWorkerManager() {
    cleanupWorker(true);
}

std::string PredictiveWorkerManager::generateSocketPath(
    const std::string& model_id,
    const std::string& process_type)
{
    static std::mt19937_64 rng(std::random_device{}());
    std::ostringstream oss;
    oss << "/tmp/conv-worker-" << process_type << "-"
        << model_id << "-" << std::hex << rng() << ".sock";
    return oss.str();
}

void PredictiveWorkerManager::ensureWorkerRunning(
    const std::string& model_id,
    const json&        init_params)
{
    // If model changed or worker crashed, restart
    if (current_model_id_ != model_id || !isWorkerRunning()) {
        cleanupWorker(true);
        startWorker(model_id, init_params);
    }
}

void PredictiveWorkerManager::startWorker(
    const std::string& model_id,
    const json&        init_params)
{
    socket_path_ = generateSocketPath(model_id, process_type_);

    // Create Unix socket pair for IPC
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        throw std::runtime_error("[PredictiveWorkerManager] socketpair failed: "
                                 + std::string(strerror(errno)));

    pid_t pid = fork();
    if (pid < 0) {
        close(sv[0]); close(sv[1]);
        throw std::runtime_error("[PredictiveWorkerManager] fork failed");
    }

    if (pid == 0) {
        // Child process
        close(sv[0]);

        // Pass socket fd via environment variable
        std::string fd_str = std::to_string(sv[1]);
        setenv("CONV_SOCKET_FD", fd_str.c_str(), 1);

        // Exec worker binary
        execl(worker_binary_.c_str(), worker_binary_.c_str(), nullptr);
        _exit(1);  // exec failed
    }

    // Parent process
    close(sv[1]);
    sock_fd_    = sv[0];
    worker_pid_ = pid;

    // Wait for initial READY
    try {
        json msg = readMessage(30);
        if (msg.value("type", "") != "READY")
            throw std::runtime_error("Expected READY, got: " + msg.dump());
    } catch (const std::exception& e) {
        cleanupWorker(true);
        throw std::runtime_error(std::string("[PredictiveWorkerManager] worker startup failed: ")
                                 + e.what());
    }

    // Send INIT command
    json init_cmd = init_params;
    init_cmd["type"]     = "INIT";
    init_cmd["model_id"] = model_id;
    sendMessage(init_cmd);

    // Wait for READY after INIT
    try {
        json msg = readMessage(60);  // model loading can take time
        if (msg.value("type", "") != "READY") {
            std::string err = msg.value("message", "unknown error");
            cleanupWorker(true);
            throw std::runtime_error("[PredictiveWorkerManager] INIT failed: " + err);
        }
    } catch (const std::exception& e) {
        cleanupWorker(true);
        throw;
    }

    current_model_id_ = model_id;
    LOG_INFO("[PredictiveWorkerManager] Worker started for model: " << model_id
             << " (pid=" << worker_pid_ << ")");
}

void PredictiveWorkerManager::cleanupWorker(bool force) {
    if (worker_pid_ > 0) {
        if (force) {
            kill(worker_pid_, SIGKILL);
        } else if (sock_fd_ >= 0) {
            try {
                sendMessage({{"type", "SHUTDOWN"}});
            } catch (...) {}
        }
        waitpid(worker_pid_, nullptr, 0);
        worker_pid_ = -1;
    }
    if (sock_fd_ >= 0) {
        close(sock_fd_);
        sock_fd_ = -1;
    }
    current_model_id_.clear();
}

void PredictiveWorkerManager::sendMessage(const json& msg) {
    std::string line = msg.dump() + "\n";
    ssize_t written = write(sock_fd_, line.c_str(), line.size());
    if (written != (ssize_t)line.size())
        throw std::runtime_error("[PredictiveWorkerManager] write failed");
}

json PredictiveWorkerManager::readMessage(int timeout_seconds) {
    std::string line;
    char ch;

    fd_set fds;
    struct timeval tv;

    while (true) {
        FD_ZERO(&fds);
        FD_SET(sock_fd_, &fds);
        tv.tv_sec  = timeout_seconds;
        tv.tv_usec = 0;

        int ret = select(sock_fd_ + 1, &fds, nullptr, nullptr, &tv);
        if (ret == 0)
            throw std::runtime_error("[PredictiveWorkerManager] read timeout");
        if (ret < 0)
            throw std::runtime_error("[PredictiveWorkerManager] select error");

        ssize_t n = read(sock_fd_, &ch, 1);
        if (n <= 0)
            throw std::runtime_error("[PredictiveWorkerManager] worker disconnected");

        if (ch == '\n') break;
        line += ch;
    }

    return json::parse(line);
}

void PredictiveWorkerManager::executeInfer(
    const std::string&            event_id,
    const TensorInferenceRequest& request,
    PredictiveResultCallback    on_result,
    PredictiveErrorCallback     on_error)
{
    // Build EXECUTE command with base64-encoded input tensors
    json inputs_json = json::array();
    for (const auto& tensor : request.inputs) {
        json t;
        t["name"]     = tensor.name;
        t["dtype"]    = tensorDataTypeToString(tensor.dtype);
        t["data_b64"] = base64Encode(tensor.data.data(), tensor.data.size());

        json shape_arr = json::array();
        for (auto d : tensor.shape) shape_arr.push_back(d);
        t["shape"] = shape_arr;

        inputs_json.push_back(t);
    }

    json output_names = json::array();
    for (const auto& name : request.output_names)
        output_names.push_back(name);

    json exec_cmd = {
        {"type",         "EXECUTE"},
        {"event_id",     event_id},
        {"model",        request.model},
        {"inputs",       inputs_json},
        {"output_names", output_names}
    };

    try {
        sendMessage(exec_cmd);

        // Wait for RESULT or ERROR
        json response = readMessage(60);
        std::string resp_type = response.value("type", "");

        if (resp_type == "ERROR") {
            on_error(response.value("message", "unknown error"));
            return;
        }

        if (resp_type != "RESULT") {
            on_error("Unexpected response type: " + resp_type);
            return;
        }

        // Decode output tensors
        TensorInferenceResponse result;
        result.model      = request.model;
        result.request_id = event_id;

        for (const auto& t : response.value("outputs", json::array())) {
            OutputTensor out;
            out.name  = t.value("name", "");
            out.dtype = tensorDataTypeFromString(t.value("dtype", "FP32"));

            for (auto d : t.value("shape", json::array()))
                out.shape.push_back(d.get<int64_t>());

            std::string b64 = t.value("data_b64", "");
            out.data = base64Decode(b64);

            result.outputs.push_back(std::move(out));
        }

        on_result(result);

    } catch (const std::exception& e) {
        on_error(std::string("IPC error: ") + e.what());
        // Worker may have crashed — mark as dead
        cleanupWorker(true);
    }
}

void PredictiveWorkerManager::terminateWorker(bool force) {
    cleanupWorker(force);
}

void PredictiveWorkerManager::shutdown() {
    cleanupWorker(false);
}

bool PredictiveWorkerManager::isWorkerRunning() const {
    if (worker_pid_ <= 0 || sock_fd_ < 0) return false;
    // Check if process is still alive
    return kill(worker_pid_, 0) == 0;
}

std::string PredictiveWorkerManager::getCurrentModelId() const {
    return current_model_id_;
}
