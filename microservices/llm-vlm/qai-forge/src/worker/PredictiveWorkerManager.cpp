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
#include <sys/mman.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>

#include <sstream>
#include <stdexcept>
#include <cstring>
#include <random>
#include <iomanip>
#include <algorithm>

namespace {
// Every tensor's start offset within its shm half is rounded up to this
// boundary — required by SNPE's ExecuteUserBuffers (unaligned -> error 407),
// harmless for QNN. Applied uniformly so pointers into the mmap'd region can
// be handed to either engine directly, with no intermediate aligned buffer.
constexpr size_t kShmAlign = 128;

size_t alignUp(size_t offset, size_t align) {
    return (offset + align - 1) / align * align;
}

constexpr size_t kDefaultShmDirBytes = 32u * 1024 * 1024;  // 32MB per direction
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// PredictiveWorkerManager
// ─────────────────────────────────────────────────────────────────────────────

PredictiveWorkerManager::PredictiveWorkerManager(
    const std::string& worker_binary,
    const std::string& process_type)
    : worker_binary_(worker_binary)
    , process_type_(process_type)
{
    shm_dir_bytes_ = kDefaultShmDirBytes;
    if (const char* env = std::getenv("GENAI_PREDICTIVE_SHM_BYTES")) {
        size_t bytes = std::strtoull(env, nullptr, 10);
        if (bytes > 0) shm_dir_bytes_ = bytes;
    }
}

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

    // Create the zero-copy tensor shared-memory region before fork() so both
    // sides inherit the same fd number across fork/exec (mirrors sv[] above).
    // No MFD_CLOEXEC — the fd must survive execl() in the child.
    int shm_fd = memfd_create("qai-forge-predictive-shm", 0);
    if (shm_fd < 0) {
        close(sv[0]); close(sv[1]);
        throw std::runtime_error("[PredictiveWorkerManager] memfd_create failed: "
                                 + std::string(strerror(errno)));
    }
    size_t shm_total_bytes = 2 * shm_dir_bytes_;
    if (ftruncate(shm_fd, static_cast<off_t>(shm_total_bytes)) < 0) {
        close(shm_fd); close(sv[0]); close(sv[1]);
        throw std::runtime_error("[PredictiveWorkerManager] ftruncate failed: "
                                 + std::string(strerror(errno)));
    }
    void* shm_ptr = mmap(nullptr, shm_total_bytes, PROT_READ | PROT_WRITE,
                         MAP_SHARED, shm_fd, 0);
    if (shm_ptr == MAP_FAILED) {
        close(shm_fd); close(sv[0]); close(sv[1]);
        throw std::runtime_error("[PredictiveWorkerManager] mmap failed: "
                                 + std::string(strerror(errno)));
    }

    pid_t pid = fork();
    if (pid < 0) {
        munmap(shm_ptr, shm_total_bytes); close(shm_fd);
        close(sv[0]); close(sv[1]);
        throw std::runtime_error("[PredictiveWorkerManager] fork failed");
    }

    if (pid == 0) {
        // Child process
        close(sv[0]);

        // Pass socket fd and shm fd/size via environment variables — same
        // fd-number-plus-env-var convention for both.
        setenv("CONV_SOCKET_FD", std::to_string(sv[1]).c_str(), 1);
        setenv("CONV_SHM_FD", std::to_string(shm_fd).c_str(), 1);
        setenv("CONV_SHM_DIR_BYTES", std::to_string(shm_dir_bytes_).c_str(), 1);

        // Exec worker binary
        execl(worker_binary_.c_str(), worker_binary_.c_str(), nullptr);
        _exit(1);  // exec failed
    }

    // Parent process
    close(sv[1]);
    sock_fd_    = sv[0];
    worker_pid_ = pid;
    shm_ptr_    = shm_ptr;
    shm_fd_     = shm_fd;

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
    if (shm_ptr_ != nullptr) {
        munmap(shm_ptr_, 2 * shm_dir_bytes_);
        shm_ptr_ = nullptr;
    }
    if (shm_fd_ >= 0) {
        close(shm_fd_);
        shm_fd_ = -1;
    }
    read_buf_.clear();
    current_model_id_.clear();
}

void PredictiveWorkerManager::sendMessage(const json& msg) {
    std::string line = msg.dump() + "\n";
    ssize_t written = write(sock_fd_, line.c_str(), line.size());
    if (written != (ssize_t)line.size())
        throw std::runtime_error("[PredictiveWorkerManager] write failed");
}

json PredictiveWorkerManager::readMessage(int timeout_seconds) {
    // Drain a large chunk per recv() instead of one byte per syscall — a
    // multi-MB base64 RESULT line can otherwise cost millions of
    // select()+read() pairs and dominate the request's wall-clock time.
    char chunk[65536];

    while (true) {
        size_t newline_pos = read_buf_.find('\n');
        if (newline_pos != std::string::npos) {
            std::string line = read_buf_.substr(0, newline_pos);
            read_buf_.erase(0, newline_pos + 1);
            return json::parse(line);
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock_fd_, &fds);
        struct timeval tv{timeout_seconds, 0};

        int ret = select(sock_fd_ + 1, &fds, nullptr, nullptr, &tv);
        if (ret == 0)
            throw std::runtime_error("[PredictiveWorkerManager] read timeout");
        if (ret < 0)
            throw std::runtime_error("[PredictiveWorkerManager] select error");

        ssize_t n = read(sock_fd_, chunk, sizeof(chunk));
        if (n <= 0)
            throw std::runtime_error("[PredictiveWorkerManager] worker disconnected");

        read_buf_.append(chunk, static_cast<size_t>(n));
    }
}

void PredictiveWorkerManager::executeInfer(
    const std::string&            event_id,
    const TensorInferenceRequest& request,
    PredictiveResultCallback    on_result,
    PredictiveErrorCallback     on_error)
{
    // Build EXECUTE command — input tensor bytes are memcpy'd into the shm
    // input half at 128-byte-aligned offsets; JSON carries only a data_ref
    // {offset,len} pointing into that region instead of the bytes themselves.
    json inputs_json = json::array();
    size_t write_offset = 0;
    for (const auto& tensor : request.inputs) {
        size_t aligned_offset = alignUp(write_offset, kShmAlign);
        size_t len = tensor.data.size();
        if (aligned_offset + len > shm_dir_bytes_) {
            on_error("Input tensor '" + tensor.name + "' exceeds shared-memory "
                     "capacity (" + std::to_string(shm_dir_bytes_) + " bytes per direction)");
            return;
        }
        std::memcpy(static_cast<uint8_t*>(shm_ptr_) + aligned_offset,
                    tensor.data.data(), len);

        json t;
        t["name"]     = tensor.name;
        t["dtype"]    = tensorDataTypeToString(tensor.dtype);
        t["data_ref"] = {{"offset", aligned_offset}, {"len", len}};

        json shape_arr = json::array();
        for (auto d : tensor.shape) shape_arr.push_back(d);
        t["shape"] = shape_arr;

        inputs_json.push_back(t);
        write_offset = aligned_offset + len;
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

        // Copy output tensors out of the shm output half.
        // request_id is Layer 1's client-facing ID (custom, or generated if the
        // client left it empty) — event_id is only for worker IPC correlation
        // and must not leak into the response in its place.
        TensorInferenceResponse result;
        result.model      = request.model;
        result.request_id = request.request_id;

        for (const auto& t : response.value("outputs", json::array())) {
            OutputTensor out;
            out.name  = t.value("name", "");
            out.dtype = tensorDataTypeFromString(t.value("dtype", "FP32"));

            for (auto d : t.value("shape", json::array()))
                out.shape.push_back(d.get<int64_t>());

            const json& ref = t.at("data_ref");
            size_t offset = ref.value("offset", (size_t)0);
            size_t len    = ref.value("len", (size_t)0);
            if (offset + len > 2 * shm_dir_bytes_)
                throw std::runtime_error("output data_ref out of bounds");

            out.data.resize(len);
            std::memcpy(out.data.data(), static_cast<uint8_t*>(shm_ptr_) + offset, len);

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

    // Poll the IPC socket instead of signaling the pid: when the worker exits
    // (crash, or its own idle self-timeout) the kernel closes its end of the
    // socketpair, which surfaces here as POLLHUP/POLLERR without blocking.
    struct pollfd pfd{sock_fd_, POLLIN, 0};
    int ret = poll(&pfd, 1, 0);
    if (ret > 0 && (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)))
        return false;

    return true;
}

std::string PredictiveWorkerManager::getCurrentModelId() const {
    return current_model_id_;
}
