// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/worker/PredictiveWorkerChannel.h"
#include "qai_forge/utils/Logger.h"

#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <cstring>
#include <stdexcept>

// ─────────────────────────────────────────────────────────────────────────────
// PredictiveWorkerChannel
// ─────────────────────────────────────────────────────────────────────────────

PredictiveWorkerChannel::PredictiveWorkerChannel(std::string worker_binary,
                                                  std::string process_type)
    : worker_binary_(std::move(worker_binary))
    , process_type_(std::move(process_type))
{
}

PredictiveWorkerChannel::~PredictiveWorkerChannel() {
    terminate();
}

// ─────────────────────────────────────────────────────────────────────────────
// Process lifecycle
// ─────────────────────────────────────────────────────────────────────────────

bool PredictiveWorkerChannel::isAlive() const noexcept {
    std::lock_guard<std::mutex> lock(process_mutex_);
    if (worker_pid_ <= 0 || sock_fd_ < 0) return false;

    struct pollfd pfd{sock_fd_, POLLIN, 0};
    int ret = poll(&pfd, 1, 0);
    if (ret > 0 && (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)))
        return false;
    return true;
}

void PredictiveWorkerChannel::ensureProcessRunningLocked() {
    // Caller holds process_mutex_.
    if (worker_pid_ > 0 && sock_fd_ >= 0) {
        struct pollfd pfd{sock_fd_, POLLIN, 0};
        int ret = poll(&pfd, 1, 0);
        const bool dead = (ret > 0 && (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)));
        if (!dead) return;
        LOG_ERROR("[PredictiveWorkerChannel:" << process_type_
                  << "] Detected dead worker process — respawning");
        cleanupProcessLocked(true);
    }
    spawnProcessLocked();
}

void PredictiveWorkerChannel::spawnProcessLocked() {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        throw std::runtime_error(
            "[PredictiveWorkerChannel] socketpair failed: " +
            std::string(strerror(errno)));
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(sv[0]);
        close(sv[1]);
        throw std::runtime_error("[PredictiveWorkerChannel] fork failed");
    }

    if (pid == 0) {
        // Child process
        close(sv[0]);
        setenv("CONV_SOCKET_FD", std::to_string(sv[1]).c_str(), 1);
        execl(worker_binary_.c_str(), worker_binary_.c_str(), nullptr);
        _exit(1);  // exec failed
    }

    // Parent
    close(sv[1]);
    sock_fd_ = sv[0];
    worker_pid_ = pid;
    read_buf_.clear();

    // Wait for initial READY (no model_id yet — this is the process-level
    // startup handshake, distinct from any per-model LOAD_MODEL/READY).
    std::string line;
    if (!readLineFromSocket(line)) {
        cleanupProcessLocked(true);
        throw std::runtime_error(
            "[PredictiveWorkerChannel] worker exited before startup READY");
    }
    json msg;
    try {
        msg = json::parse(line);
    } catch (const std::exception& e) {
        cleanupProcessLocked(true);
        throw std::runtime_error(
            std::string("[PredictiveWorkerChannel] malformed startup message: ") +
            e.what());
    }
    if (msg.value("type", "") != "READY") {
        cleanupProcessLocked(true);
        throw std::runtime_error(
            "[PredictiveWorkerChannel] expected startup READY, got: " + line);
    }

    // Launch the reader thread now that the process is confirmed alive.
    reader_running_ = true;
    reader_thread_ = std::thread(&PredictiveWorkerChannel::readerLoop, this);

    LOG_INFO("[PredictiveWorkerChannel:" << process_type_
             << "] Worker process started (pid=" << worker_pid_ << ")");
}

void PredictiveWorkerChannel::cleanupProcessLocked(bool force) noexcept {
    // Caller holds process_mutex_.
    reader_running_ = false;

    if (sock_fd_ >= 0) {
        if (!force) {
            try {
                json shutdown_msg = {{"type", "SHUTDOWN"}};
                sendMessageLocked(shutdown_msg);
            } catch (...) {
            }
        }
        // Shut down the socket so the reader thread's blocking read()/select()
        // wakes up with EOF/error instead of hanging until process exit.
        ::shutdown(sock_fd_, SHUT_RDWR);
    }

    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }

    if (worker_pid_ > 0) {
        if (force) {
            kill(worker_pid_, SIGKILL);
        }
        int status = 0;
        waitpid(worker_pid_, &status, 0);
        worker_pid_ = -1;
    }

    if (sock_fd_ >= 0) {
        close(sock_fd_);
        sock_fd_ = -1;
    }
    read_buf_.clear();

    // Fail any calls still waiting — the process is gone, they will never
    // get a response.
    {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        json failure = {{"type", "ERROR"}, {"message", "worker process terminated"}};
        for (auto& [key, call] : model_calls_) {
            std::lock_guard<std::mutex> call_lock(call->mutex);
            call->response = failure;
            call->done = true;
            call->cv.notify_all();
        }
        for (auto& [key, call] : event_calls_) {
            std::lock_guard<std::mutex> call_lock(call->mutex);
            call->response = failure;
            call->done = true;
            call->cv.notify_all();
        }
        model_calls_.clear();
        event_calls_.clear();
    }
}

void PredictiveWorkerChannel::terminate() noexcept {
    shutting_down_ = true;
    std::lock_guard<std::mutex> lock(process_mutex_);
    cleanupProcessLocked(true);
}

void PredictiveWorkerChannel::shutdown() noexcept {
    shutting_down_ = true;
    std::lock_guard<std::mutex> lock(process_mutex_);
    cleanupProcessLocked(false);
}

// ─────────────────────────────────────────────────────────────────────────────
// Low-level I/O
// ─────────────────────────────────────────────────────────────────────────────

void PredictiveWorkerChannel::sendMessageLocked(const json& msg) {
    // Caller holds process_mutex_ OR is the initial startup path where no
    // concurrent writers exist yet. All post-startup callers must go through
    // write_mutex_ via the public methods below.
    std::string line = msg.dump() + "\n";
    ssize_t written = write(sock_fd_, line.c_str(), line.size());
    if (written != static_cast<ssize_t>(line.size())) {
        throw std::runtime_error("[PredictiveWorkerChannel] write failed");
    }
}

void PredictiveWorkerChannel::sendMessageWithFdLocked(const json& msg, int fd) {
    std::string line = msg.dump() + "\n";

    struct iovec iov;
    iov.iov_base = const_cast<char*>(line.data());
    iov.iov_len = line.size();

    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    std::memset(cmsg_buf, 0, sizeof(cmsg_buf));

    struct msghdr msg_hdr;
    std::memset(&msg_hdr, 0, sizeof(msg_hdr));
    msg_hdr.msg_iov = &iov;
    msg_hdr.msg_iovlen = 1;
    msg_hdr.msg_control = cmsg_buf;
    msg_hdr.msg_controllen = sizeof(cmsg_buf);

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg_hdr);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    ssize_t sent = sendmsg(sock_fd_, &msg_hdr, 0);
    if (sent != static_cast<ssize_t>(line.size())) {
        throw std::runtime_error(
            "[PredictiveWorkerChannel] sendmsg with fd failed: " +
            std::string(strerror(errno)));
    }
}

bool PredictiveWorkerChannel::readLineFromSocket(std::string& out_line) {
    char chunk[65536];
    while (true) {
        size_t newline_pos = read_buf_.find('\n');
        if (newline_pos != std::string::npos) {
            out_line = read_buf_.substr(0, newline_pos);
            read_buf_.erase(0, newline_pos + 1);
            return true;
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock_fd_, &fds);
        struct timeval tv{5, 0};  // short poll so shutdown()/reader stop is responsive
        int ret = select(sock_fd_ + 1, &fds, nullptr, nullptr, &tv);
        if (ret == 0) {
            if (!reader_running_ && read_buf_.empty()) return false;
            continue;  // timeout — loop back and check reader_running_/select again
        }
        if (ret < 0) return false;

        ssize_t n = read(sock_fd_, chunk, sizeof(chunk));
        if (n <= 0) return false;  // EOF or error
        read_buf_.append(chunk, static_cast<size_t>(n));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Reader thread — demultiplexes worker responses to blocked callers
// ─────────────────────────────────────────────────────────────────────────────

void PredictiveWorkerChannel::readerLoop() {
    while (reader_running_) {
        std::string line;
        if (!readLineFromSocket(line)) {
            break;  // socket closed/error — process considered dead
        }

        json msg;
        try {
            msg = json::parse(line);
        } catch (const std::exception& e) {
            LOG_ERROR("[PredictiveWorkerChannel:" << process_type_
                      << "] Failed to parse worker message: " << e.what());
            continue;
        }

        const std::string type = msg.value("type", "");
        const bool is_model_scoped =
            (type == "READY" || type == "ERROR" || type == "UNLOADED") &&
            msg.contains("model_id") && !msg.contains("event_id");
        const bool is_event_scoped = msg.contains("event_id");

        std::shared_ptr<PendingCall> call;
        if (is_event_scoped) {
            const std::string event_id = msg.value("event_id", "");
            std::lock_guard<std::mutex> lock(calls_mutex_);
            auto it = event_calls_.find(event_id);
            if (it != event_calls_.end()) {
                call = it->second;
                event_calls_.erase(it);
            }
        } else if (is_model_scoped) {
            const std::string model_id = msg.value("model_id", "");
            std::lock_guard<std::mutex> lock(calls_mutex_);
            auto it = model_calls_.find(model_id);
            if (it != model_calls_.end()) {
                call = it->second;
                model_calls_.erase(it);
            }
        } else {
            LOG_ERROR("[PredictiveWorkerChannel:" << process_type_
                      << "] Received unroutable message: " << line);
            continue;
        }

        if (!call) {
            LOG_ERROR("[PredictiveWorkerChannel:" << process_type_
                      << "] No pending caller for message: " << line);
            continue;
        }

        std::lock_guard<std::mutex> call_lock(call->mutex);
        call->response = std::move(msg);
        call->done = true;
        call->cv.notify_all();
    }

    // If we fall out of the loop because the socket died (not because
    // shutdown() cleared reader_running_ deliberately), fail all pending
    // calls so blocked callers don't hang until their own read timeouts.
    if (!shutting_down_) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        json failure = {{"type", "ERROR"}, {"message", "worker connection lost"}};
        for (auto& [key, call] : model_calls_) {
            std::lock_guard<std::mutex> call_lock(call->mutex);
            call->response = failure;
            call->done = true;
            call->cv.notify_all();
        }
        for (auto& [key, call] : event_calls_) {
            std::lock_guard<std::mutex> call_lock(call->mutex);
            call->response = failure;
            call->done = true;
            call->cv.notify_all();
        }
        model_calls_.clear();
        event_calls_.clear();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

void PredictiveWorkerChannel::loadModel(
    const std::string& model_id,
    const json& init_params,
    int shm_fd,
    void* /*shm_ptr*/,
    size_t shm_dir_bytes,
    PredictiveChannelReadyValidator ready_validator)
{
    auto call = std::make_shared<PendingCall>();
    {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        model_calls_[model_id] = call;
    }

    try {
        std::lock_guard<std::mutex> process_lock(process_mutex_);
        ensureProcessRunningLocked();

        json load_cmd = init_params;
        load_cmd["type"] = "LOAD_MODEL";
        load_cmd["model_id"] = model_id;
        load_cmd["shm_dir_bytes"] = shm_dir_bytes;

        std::lock_guard<std::mutex> write_lock(write_mutex_);
        sendMessageWithFdLocked(load_cmd, shm_fd);
    } catch (const std::exception&) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        model_calls_.erase(model_id);
        throw;
    }

    std::unique_lock<std::mutex> call_lock(call->mutex);
    const bool got_response = call->cv.wait_for(
        call_lock, std::chrono::seconds(60), [&call]() { return call->done; });

    if (!got_response) {
        {
            std::lock_guard<std::mutex> lock(calls_mutex_);
            model_calls_.erase(model_id);
        }
        throw std::runtime_error(
            "[PredictiveWorkerChannel] timed out waiting for LOAD_MODEL READY: " +
            model_id);
    }

    const json response = call->response;
    call_lock.unlock();

    if (response.value("type", "") != "READY") {
        throw std::runtime_error(
            "[PredictiveWorkerChannel] LOAD_MODEL failed for '" + model_id +
            "': " + response.value("message", "unknown error"));
    }

    if (ready_validator) {
        ready_validator(response);
    }
}

void PredictiveWorkerChannel::unloadModel(const std::string& model_id) noexcept {
    if (!isAlive()) return;

    auto call = std::make_shared<PendingCall>();
    {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        model_calls_[model_id] = call;
    }

    try {
        json unload_cmd = {{"type", "UNLOAD_MODEL"}, {"model_id", model_id}};
        std::lock_guard<std::mutex> write_lock(write_mutex_);
        {
            std::lock_guard<std::mutex> process_lock(process_mutex_);
            if (sock_fd_ < 0) {
                std::lock_guard<std::mutex> lock(calls_mutex_);
                model_calls_.erase(model_id);
                return;
            }
            sendMessageLocked(unload_cmd);
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        model_calls_.erase(model_id);
        return;
    }

    std::unique_lock<std::mutex> call_lock(call->mutex);
    call->cv.wait_for(call_lock, std::chrono::seconds(10),
                       [&call]() { return call->done; });
    // Best-effort — do not throw from a noexcept unload path even on
    // timeout/ERROR; the model is being torn down regardless.
    {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        model_calls_.erase(model_id);
    }
}

PredictiveWorkerChannel::ExecuteResult PredictiveWorkerChannel::executeInfer(
    const std::string& /*model_id*/,
    const std::string& event_id,
    const json& execute_message,
    int timeout_seconds)
{
    ExecuteResult result;

    auto call = std::make_shared<PendingCall>();
    {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        event_calls_[event_id] = call;
    }

    try {
        std::lock_guard<std::mutex> write_lock(write_mutex_);
        std::lock_guard<std::mutex> process_lock(process_mutex_);
        if (sock_fd_ < 0) {
            throw std::runtime_error("worker process is not running");
        }
        sendMessageLocked(execute_message);
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        event_calls_.erase(event_id);
        result.ok = false;
        result.error_message = e.what();
        return result;
    }

    std::unique_lock<std::mutex> call_lock(call->mutex);
    const bool got_response = call->cv.wait_for(
        call_lock, std::chrono::seconds(timeout_seconds),
        [&call]() { return call->done; });

    if (!got_response) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        event_calls_.erase(event_id);
        result.ok = false;
        result.error_message = "timed out waiting for RESULT";
        return result;
    }

    const json response = call->response;
    call_lock.unlock();

    if (response.value("type", "") != "RESULT") {
        result.ok = false;
        result.error_message = response.value("message", "unknown error");
        return result;
    }

    result.ok = true;
    result.outputs = response.value("outputs", json::array());
    return result;
}
