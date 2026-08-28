// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// InferenceWorkerManager — Layer 3 Implementation
//
// Manages the genai-inference-worker subprocess lifecycle and communicates
// via JSON Lines over a Unix Domain Socket (socketpair).
//
// Architecture compliance (architecture_refactoring_design.md Section 4):
//   A. No threading + queue: socket reads are synchronous within executeRequest().
//      The Drogon thread pool handles concurrency at Layer 1.
//   B. Typed IPC events: all responses are parsed into IPCTokenEvent etc.
//   C. bypass_think_filter: passed through to the worker via EXECUTE command.
//   D. No ADHOC_MODE: sendReset() is called explicitly by Layer 2.
//   E. UUID socket paths: prevents clashes between concurrent workers.
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/worker/InferenceWorkerManager.h"
#include "qai_forge/utils/Logger.h"
#include <sstream>
#include <iomanip>
#include <random>
#include <stdexcept>
#include <cstring>
#include <cerrno>
#include <chrono>

// POSIX headers for subprocess and socket management
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/mman.h>

namespace {
constexpr size_t kDefaultPromptShmBytes = 4u * 1024 * 1024;   // 4MB
constexpr size_t kDefaultImageShmBytes  = 32u * 1024 * 1024;  // 32MB
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────
InferenceWorkerManager::InferenceWorkerManager(const std::string& process_type)
    : process_type_(process_type) {
    // Read the active-inference silence threshold once at startup.
    auto read_env_int = [](const char* name, int default_val) -> int {
        const char* val = std::getenv(name);
        if (!val) return default_val;
        try { return std::stoi(val); } catch (...) { return default_val; }
    };
    active_timeout_seconds_ = read_env_int("GENAI_WORKER_ACTIVE_TIMEOUT", 600);
    if (active_timeout_seconds_ <= 0) {
        active_timeout_seconds_ = 600;
    }

    prompt_shm_bytes_ = kDefaultPromptShmBytes;
    if (const char* env = std::getenv("GENAI_PROMPT_SHM_BYTES")) {
        size_t bytes = std::strtoull(env, nullptr, 10);
        if (bytes > 0) prompt_shm_bytes_ = bytes;
    }

    image_shm_bytes_ = kDefaultImageShmBytes;
    if (const char* env = std::getenv("GENAI_IMAGE_SHM_BYTES")) {
        size_t bytes = std::strtoull(env, nullptr, 10);
        if (bytes > 0) image_shm_bytes_ = bytes;
    }

}

InferenceWorkerManager::~InferenceWorkerManager() {
    shutdown();
}

// ─────────────────────────────────────────────────────────────────────────────
// generateSocketPath — Section 4.E: UUID suffix prevents clashes
// ─────────────────────────────────────────────────────────────────────────────
std::string InferenceWorkerManager::generateSocketPath(const std::string& model_id) {
    static std::mt19937_64 rng(std::random_device{}());
    static std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss;
    oss << "/tmp/genai-worker-" << model_id << "-"
        << std::hex << std::setw(16) << std::setfill('0') << dist(rng) << ".sock";
    return oss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// ensureWorkerRunning
// ─────────────────────────────────────────────────────────────────────────────
void InferenceWorkerManager::ensureWorkerRunning(const std::string& model_id,
                                                  const std::string& config_file,
                                                  const std::string& sampler_config) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Model switch: terminate old worker and start fresh
    if (!current_model_id_.empty() && current_model_id_ != model_id) {
        LOG_INFO("[" << process_type_ << "Worker] Model switch: "
                 << current_model_id_ << " -> " << model_id);
        cleanupWorker(false);
    }

    // Start worker if not running
    if (!isWorkerRunningLocked()) {
        startWorker(model_id, config_file, sampler_config);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// startWorker — Fork/exec the genai-inference-worker binary
// ─────────────────────────────────────────────────────────────────────────────
void InferenceWorkerManager::startWorker(const std::string& model_id,
                                          const std::string& config_file,
                                          const std::string& sampler_config) {
    // Create a socketpair for bidirectional IPC
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        throw std::runtime_error(std::string("socketpair() failed: ") + strerror(errno));
    }

    int parent_fd = sv[0];
    int child_fd  = sv[1];

    // Create the prompt shared-memory region before fork() so both sides
    // inherit the same fd number across fork/exec (mirrors sv[] above).
    // No MFD_CLOEXEC — the fd must survive execl() in the child.
    int prompt_shm_fd = memfd_create("qai-forge-prompt-shm", 0);
    if (prompt_shm_fd < 0) {
        ::close(sv[0]); ::close(sv[1]);
        throw std::runtime_error(std::string("memfd_create failed: ") + strerror(errno));
    }
    if (ftruncate(prompt_shm_fd, static_cast<off_t>(prompt_shm_bytes_)) < 0) {
        ::close(prompt_shm_fd); ::close(sv[0]); ::close(sv[1]);
        throw std::runtime_error(std::string("ftruncate failed: ") + strerror(errno));
    }
    void* prompt_shm_ptr = mmap(nullptr, prompt_shm_bytes_, PROT_READ | PROT_WRITE,
                                MAP_SHARED, prompt_shm_fd, 0);
    if (prompt_shm_ptr == MAP_FAILED) {
        ::close(prompt_shm_fd); ::close(sv[0]); ::close(sv[1]);
        throw std::runtime_error(std::string("mmap failed: ") + strerror(errno));
    }

    // Create the image shared-memory region (VLM only) — mirrors the prompt
    // region above. LLM/litert-lm workers never see this fd/env var.
    int   image_shm_fd  = -1;
    void* image_shm_ptr = nullptr;
    const bool is_vlm_worker = (process_type_ == "vlm");
    if (is_vlm_worker) {
        image_shm_fd = memfd_create("qai-forge-image-shm", 0);
        if (image_shm_fd < 0) {
            munmap(prompt_shm_ptr, prompt_shm_bytes_); ::close(prompt_shm_fd);
            ::close(sv[0]); ::close(sv[1]);
            throw std::runtime_error(std::string("memfd_create (image) failed: ") + strerror(errno));
        }
        if (ftruncate(image_shm_fd, static_cast<off_t>(image_shm_bytes_)) < 0) {
            ::close(image_shm_fd);
            munmap(prompt_shm_ptr, prompt_shm_bytes_); ::close(prompt_shm_fd);
            ::close(sv[0]); ::close(sv[1]);
            throw std::runtime_error(std::string("ftruncate (image) failed: ") + strerror(errno));
        }
        image_shm_ptr = mmap(nullptr, image_shm_bytes_, PROT_READ | PROT_WRITE,
                             MAP_SHARED, image_shm_fd, 0);
        if (image_shm_ptr == MAP_FAILED) {
            ::close(image_shm_fd);
            munmap(prompt_shm_ptr, prompt_shm_bytes_); ::close(prompt_shm_fd);
            ::close(sv[0]); ::close(sv[1]);
            throw std::runtime_error(std::string("mmap (image) failed: ") + strerror(errno));
        }
    }

    // Determine worker binary path from environment.
    // Each process type has its own binary and socket FD env var.
    const char* worker_bin_env = nullptr;
    const char* default_bin    = nullptr;
    std::string socket_fd_var;

    if (process_type_ == "vlm") {
        worker_bin_env = std::getenv("GENAI_VLM_WORKER_BINARY");
        default_bin    = "/usr/local/bin/genai-vlm-inference-worker";
        socket_fd_var  = "VLM_SOCKET_FD=" + std::to_string(child_fd);
    } else if (process_type_ == "litert-lm") {
        worker_bin_env = std::getenv("LITERT_LM_WORKER_BINARY");
        default_bin    = "/usr/local/bin/litert-lm-inference-worker";
        socket_fd_var  = "LLM_SOCKET_FD=" + std::to_string(child_fd);
    } else {
        // Default: LLM (genai-inference-worker)
        worker_bin_env = std::getenv("GENAI_WORKER_BINARY");
        default_bin    = "/usr/local/bin/genai-inference-worker";
        socket_fd_var  = "LLM_SOCKET_FD=" + std::to_string(child_fd);
    }
    const char* worker_bin = worker_bin_env ? worker_bin_env : default_bin;

    // Set the child socket FD in the environment for the worker
    std::string child_fd_str = std::to_string(child_fd);
    std::string env_var = socket_fd_var;
    std::string prompt_shm_fd_var    = "PROMPT_SHM_FD=" + std::to_string(prompt_shm_fd);
    std::string prompt_shm_bytes_var = "PROMPT_SHM_BYTES=" + std::to_string(prompt_shm_bytes_);
    std::string image_shm_fd_var     = "IMAGE_SHM_FD=" + std::to_string(image_shm_fd);
    std::string image_shm_bytes_var  = "IMAGE_SHM_BYTES=" + std::to_string(image_shm_bytes_);

    pid_t pid = ::fork();
    if (pid < 0) {
        if (is_vlm_worker) { munmap(image_shm_ptr, image_shm_bytes_); ::close(image_shm_fd); }
        munmap(prompt_shm_ptr, prompt_shm_bytes_); ::close(prompt_shm_fd);
        ::close(sv[0]);
        ::close(sv[1]);
        throw std::runtime_error(std::string("fork() failed: ") + strerror(errno));
    }

    if (pid == 0) {
        // ── Child process ──────────────────────────────────────────────────
        ::close(parent_fd);

        // Set socket FD and prompt shm FD/size environment variables
        ::putenv(const_cast<char*>(env_var.c_str()));
        ::putenv(const_cast<char*>(prompt_shm_fd_var.c_str()));
        ::putenv(const_cast<char*>(prompt_shm_bytes_var.c_str()));
        if (is_vlm_worker) {
            ::putenv(const_cast<char*>(image_shm_fd_var.c_str()));
            ::putenv(const_cast<char*>(image_shm_bytes_var.c_str()));
        }

        // Exec the worker binary
        ::execl(worker_bin, worker_bin, nullptr);

        // If exec fails — use write() directly; Logger mutex may be locked in parent
        const char msg[] = "[Worker] execl failed\n";
        [[maybe_unused]] ssize_t result = ::write(STDERR_FILENO, msg, sizeof(msg) - 1);
        ::_exit(1);
    }

    // ── Parent process ─────────────────────────────────────────────────────
    ::close(child_fd);
    worker_pid_ = pid;
    sock_fd_ = parent_fd;
    prompt_shm_ptr_ = prompt_shm_ptr;
    prompt_shm_fd_  = prompt_shm_fd;
    if (is_vlm_worker) {
        image_shm_ptr_ = image_shm_ptr;
        image_shm_fd_  = image_shm_fd;
    }
    socket_path_ = generateSocketPath(model_id);

    LOG_INFO("[" << process_type_ << "Worker] Started PID " << worker_pid_
             << " for model " << model_id);

    // Wait for READY from worker
    json ready = readMessage(30);
    if (ready.value("type", "") != ResponseType::READY) {
        cleanupWorker(true);
        throw std::runtime_error("Worker failed to send READY after startup");
    }

    // Send INIT command
    json init_cmd = InferenceProtocol::createInitCommand(model_id, config_file, sampler_config);
    sendMessage(init_cmd);

    // Wait for READY after INIT
    json init_ready = readMessage(60);
    if (init_ready.value("type", "") != ResponseType::READY) {
        std::string err = init_ready.value("message", "Unknown error");
        cleanupWorker(true);
        throw std::runtime_error("Worker INIT failed: " + err);
    }

    current_model_id_ = model_id;

    // Arm the watchdog: record the PID to monitor, reset the activity clock,
    // and start the background thread.
    watchdog_target_pid_.store(worker_pid_);
    last_activity_time_.store(std::chrono::steady_clock::now());
    startWatchdog();

    LOG_INFO("[" << process_type_ << "Worker] Ready for model: " << model_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// cleanupWorker
// ─────────────────────────────────────────────────────────────────────────────
void InferenceWorkerManager::cleanupWorker(bool force) {
    // Disarm the watchdog first.  The watchdog thread never acquires mutex_,
    // so joining it here (while mutex_ is held by our caller) is safe.
    stopWatchdog();
    watchdog_target_pid_.store(-1);

    if (sock_fd_ >= 0) {
        ::close(sock_fd_);
        sock_fd_ = -1;
    }

    if (worker_pid_ > 0) {
        if (force) {
            ::kill(worker_pid_, SIGKILL);
        } else {
            ::kill(worker_pid_, SIGTERM);
            // Give it 5 seconds to exit gracefully
            for (int i = 0; i < 50; ++i) {
                int status;
                pid_t result = ::waitpid(worker_pid_, &status, WNOHANG);
                if (result != 0) break;
                ::usleep(100000); // 100ms
            }
            // Force kill if still running
            if (isWorkerRunningLocked()) {
                ::kill(worker_pid_, SIGKILL);
            }
        }
        ::waitpid(worker_pid_, nullptr, 0);
        worker_pid_ = -1;
    }

    if (prompt_shm_ptr_ != nullptr) {
        munmap(prompt_shm_ptr_, prompt_shm_bytes_);
        prompt_shm_ptr_ = nullptr;
    }
    if (prompt_shm_fd_ >= 0) {
        ::close(prompt_shm_fd_);
        prompt_shm_fd_ = -1;
    }

    if (image_shm_ptr_ != nullptr) {
        munmap(image_shm_ptr_, image_shm_bytes_);
        image_shm_ptr_ = nullptr;
    }
    if (image_shm_fd_ >= 0) {
        ::close(image_shm_fd_);
        image_shm_fd_ = -1;
    }

    read_buf_.clear();
    current_model_id_.clear();
    is_active_ = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// sendMessage — Write a JSON Line to the socket
// ─────────────────────────────────────────────────────────────────────────────
void InferenceWorkerManager::sendMessage(const json& msg) {
    if (sock_fd_ < 0) throw std::runtime_error("Worker socket not connected");
    std::string line = InferenceProtocol::serialize(msg);
    ssize_t written = ::write(sock_fd_, line.c_str(), line.size());
    if (written < 0) {
        throw std::runtime_error(std::string("Socket write failed: ") + strerror(errno));
    }
    // Any outbound IPC traffic counts as activity — reset the watchdog timer.
    last_activity_time_.store(std::chrono::steady_clock::now());
}

// ─────────────────────────────────────────────────────────────────────────────
// readMessage — Read a JSON Line from the socket with timeout
// ─────────────────────────────────────────────────────────────────────────────
json InferenceWorkerManager::readMessage(int timeout_seconds) {
    if (sock_fd_ < 0) throw std::runtime_error("Worker socket not connected");

    // Drain a large chunk per recv() instead of one byte per syscall — a
    // long prompt/response line can otherwise cost many select()+read()
    // pairs and dominate the request's wall-clock time.
    char chunk[65536];

    while (true) {
        size_t newline_pos = read_buf_.find('\n');
        if (newline_pos != std::string::npos) {
            std::string line = read_buf_.substr(0, newline_pos);
            read_buf_.erase(0, newline_pos + 1);
            // Any inbound IPC traffic counts as activity — reset the watchdog timer.
            last_activity_time_.store(std::chrono::steady_clock::now());
            return InferenceProtocol::deserialize(line);
        }

        // Use select() for timeout
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(sock_fd_, &read_fds);
        struct timeval tv{timeout_seconds, 0};

        int ready = ::select(sock_fd_ + 1, &read_fds, nullptr, nullptr, &tv);
        if (ready == 0) throw std::runtime_error("Timeout waiting for worker response");
        if (ready < 0) throw std::runtime_error(std::string("select() failed: ") + strerror(errno));

        ssize_t n = ::read(sock_fd_, chunk, sizeof(chunk));
        if (n <= 0) throw std::runtime_error("Worker socket closed (EOF)");

        read_buf_.append(chunk, static_cast<size_t>(n));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// writePromptToShm — copy a prompt into the shared-memory region, return a
// {"offset","len"} reference for EXECUTE's "prompt_ref" field. Caller must
// hold mutex_. See header Section G for the region's lifecycle.
// ─────────────────────────────────────────────────────────────────────────────
json InferenceWorkerManager::writePromptToShm(const std::string& prompt) {
    if (prompt.size() > prompt_shm_bytes_) {
        throw std::runtime_error(
            "Prompt exceeds shared-memory capacity (" +
            std::to_string(prompt_shm_bytes_) + " bytes)");
    }
    if (!prompt.empty()) {
        std::memcpy(prompt_shm_ptr_, prompt.data(), prompt.size());
    }
    return {{"offset", 0}, {"len", prompt.size()}};
}

// ─────────────────────────────────────────────────────────────────────────────
// writeImagesToShm — copy each image's bytes sequentially into the image
// shared-memory region, return a JSON array of {"offset","len"} references
// for EXECUTE's "image_refs" field. Caller must hold mutex_. See header
// Section H for the region's lifecycle.
// ─────────────────────────────────────────────────────────────────────────────
json InferenceWorkerManager::writeImagesToShm(const std::vector<std::vector<uint8_t>>& images) {
    size_t total = 0;
    for (const auto& img : images) total += img.size();
    if (total > image_shm_bytes_) {
        throw std::runtime_error(
            "Images exceed shared-memory capacity (" +
            std::to_string(image_shm_bytes_) + " bytes)");
    }

    json refs = json::array();
    size_t offset = 0;
    auto* base = static_cast<uint8_t*>(image_shm_ptr_);
    for (const auto& img : images) {
        if (!img.empty()) {
            std::memcpy(base + offset, img.data(), img.size());
        }
        refs.push_back({{"offset", offset}, {"len", img.size()}});
        offset += img.size();
    }
    return refs;
}

// ─────────────────────────────────────────────────────────────────────────────
// sendCommandAndWaitReady — Send command and drain until matching READY
// ─────────────────────────────────────────────────────────────────────────────
bool InferenceWorkerManager::sendCommandAndWaitReady(const json& cmd, int timeout_seconds) {
    std::string command_id = cmd.value("command_id", "");
    sendMessage(cmd);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        json response = readMessage(timeout_seconds);
        std::string type = response.value("type", "");

        if (type == ResponseType::READY) {
            std::string resp_cmd_id = response.value("command_id", "");
            if (command_id.empty() || resp_cmd_id == command_id) return true;
            // Stale READY — keep draining
            continue;
        }
        if (type == ResponseType::ERROR) {
            LOG_ERROR("[" << process_type_ << "Worker] Command failed: "
                      << response.value("message", "Unknown"));
            return false;
        }
        // TOKEN/DONE are stale stream output — drain silently
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// sendExecuteAndStream — protected helper (Fix 4)
//
// Sends a pre-built EXECUTE command and reads TOKEN/DONE/READY/ERROR responses
// until the final READY arrives. Caller MUST hold mutex_.
//
// Extracted from executeRequest() so that VlmInferenceWorkerManager can build
// an augmented command (with image_refs) and call this method directly,
// without duplicating the streaming loop.
// ─────────────────────────────────────────────────────────────────────────────
void InferenceWorkerManager::sendExecuteAndStream(const json& execute_cmd,
                                                   const std::string& event_id,
                                                   TokenCallback on_token,
                                                   DoneCallback on_done,
                                                   ErrorCallback on_error) {
    struct ActiveStateGuard {
        explicit ActiveStateGuard(std::atomic<bool>& active)
            : active_(active) {
        }

        ~ActiveStateGuard() {
            active_.store(false, std::memory_order_release);
        }

        std::atomic<bool>& active_;
    };

    last_activity_time_.store(
        std::chrono::steady_clock::now(),
        std::memory_order_relaxed);
    is_active_.store(true, std::memory_order_release);
    const ActiveStateGuard active_state_guard(is_active_);

    sendMessage(execute_cmd);

    bool pending_error = false;
    std::string pending_error_msg;

    while (true) {
        json response;
        try {
            const int socket_fallback_seconds =
                active_timeout_seconds_ + (2 * watchdog_interval_seconds_);
            response = readMessage(socket_fallback_seconds);
        } catch (const std::exception& e) {
            on_error({event_id, "", std::string("Socket error: ") + e.what()});
            return;
        }

        std::string type = response.value("type", "");
        std::string resp_event_id = response.value("event_id", "");

        // Ignore stale messages from previous events
        if (!resp_event_id.empty() && resp_event_id != event_id) continue;

        if (type == ResponseType::TOKEN) {
            on_token(InferenceProtocol::parseToken(response));
        } else if (type == ResponseType::DONE) {
            on_done(InferenceProtocol::parseDone(response));
            // Wait for the final READY that follows DONE
        } else if (type == ResponseType::READY) {
            // Stream complete
            if (pending_error) {
                on_error({event_id, "", pending_error_msg});
            }
            break;
        } else if (type == ResponseType::ERROR) {
            // Defer error until READY arrives (to drain state cleanly)
            pending_error = true;
            pending_error_msg = response.value("message", "Unknown error");
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// executeRequest — builds EXECUTE command and delegates to sendExecuteAndStream
// ─────────────────────────────────────────────────────────────────────────────
void InferenceWorkerManager::executeRequest(const std::string& event_id,
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
                                             ErrorCallback on_error) {
    // Wait for any pending background KV reset to complete before acquiring
    // mutex_ and sending EXECUTE. In the common case the reset is already done
    // (it ran concurrently while the previous response was being sent to the
    // client), so this is a no-op with zero added latency.
    waitForPendingReset();

    std::lock_guard<std::mutex> lock(mutex_);

    LOG_DEBUG("[" << process_type_ << "Worker] executeRequest event_id=" << event_id
              << " model=" << current_model_id_ << " streaming=" << streaming
              << " max_tokens=" << max_tokens);

    json prompt_ref;
    try {
        prompt_ref = writePromptToShm(prompt);
    } catch (const std::exception& e) {
        is_active_ = false;
        on_error({event_id, "", e.what()});
        return;
    }

    json execute_cmd = InferenceProtocol::createExecuteCommand(
        event_id, prompt_ref, streaming, max_tokens, temperature,
        top_p, top_k, presence_penalty, frequency_penalty, bypass_think_filter
    );
    sendExecuteAndStream(execute_cmd, event_id, on_token, on_done, on_error);
}

void InferenceWorkerManager::executeStructuredRequest(
    const std::string& event_id,
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
    ErrorCallback on_error) {
    waitForPendingReset();

    std::lock_guard<std::mutex> lock(mutex_);

    json execute_cmd = InferenceProtocol::createStructuredExecuteCommand(
        event_id,
        messages,
        tools,
        streaming,
        max_tokens,
        temperature,
        top_p,
        top_k,
        presence_penalty,
        frequency_penalty);
    sendExecuteAndStream(execute_cmd, event_id, on_token, on_done, on_error);
}

// ─────────────────────────────────────────────────────────────────────────────
// sendReset — Called by Layer 2's ConcurrencyMiddleware (not by Layer 3 itself)
// ─────────────────────────────────────────────────────────────────────────────
void InferenceWorkerManager::sendReset() {
    std::lock_guard<std::mutex> lock(mutex_);
    static std::mt19937_64 rng(std::random_device{}());
    std::ostringstream oss;
    oss << std::hex << rng();
    std::string command_id = "reset-" + oss.str().substr(0, 8);

    json reset_cmd = InferenceProtocol::createResetCommand(command_id);
    if (!sendCommandAndWaitReady(reset_cmd, 10)) {
        throw std::runtime_error("RESET command failed or timed out");
    }
    LOG_INFO("[" << process_type_ << "Worker] KV cache reset successfully");
}

// ─────────────────────────────────────────────────────────────────────────────
// initiateBackgroundReset — Eager post-inference KV cache reset
//
// Spawns a background thread that calls sendReset() (RESET + wait for READY).
// Returns immediately so the caller can return the response to the HTTP layer
// while the reset runs concurrently.
//
// The next executeRequest() call joins this thread via waitForPendingReset()
// before acquiring mutex_, ensuring clean KV state with minimal latency.
// ─────────────────────────────────────────────────────────────────────────────
void InferenceWorkerManager::initiateBackgroundReset() {
    // Join any previous reset thread before spawning a new one.
    // In normal operation the previous reset is already done by the time
    // the next request arrives, so this join is a no-op.
    if (reset_thread_.joinable()) {
        reset_thread_.join();
    }

    reset_thread_ = std::thread([this]() {
        try {
            sendReset();
        } catch (const std::exception& e) {
            LOG_WARN("[" << process_type_ << "Worker] Background KV reset failed: "
                     << e.what() << " (non-fatal — next request will rebuild from scratch)");
        } catch (...) {
            LOG_WARN("[" << process_type_ << "Worker] Background KV reset failed "
                     "with unknown exception (non-fatal)");
        }
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// waitForPendingReset — Block until the background reset thread completes
//
// Called at the start of executeRequest() to ensure the KV cache is clean
// before sending the EXECUTE command. If no reset is in progress (thread not
// joinable), returns immediately.
// ─────────────────────────────────────────────────────────────────────────────
void InferenceWorkerManager::waitForPendingReset() {
    if (reset_thread_.joinable()) {
        LOG_INFO("[" << process_type_ << "Worker] Waiting for background KV reset to complete");
        reset_thread_.join();
        LOG_INFO("[" << process_type_ << "Worker] Background KV reset complete — ready for inference");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// saveKvCache / restoreKvCache — Section 6 (KV Cache Management)
// ─────────────────────────────────────────────────────────────────────────────
bool InferenceWorkerManager::saveKvCache(const std::string& checkpoint_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    static std::mt19937_64 rng(std::random_device{}());
    std::ostringstream oss;
    oss << std::hex << rng();
    std::string command_id = "savekv-" + oss.str().substr(0, 8);

    json cmd = InferenceProtocol::createSaveKvCommand(checkpoint_name, command_id);
    bool ok = sendCommandAndWaitReady(cmd, 30);
    if (ok) LOG_INFO("[" << process_type_ << "Worker] KV saved: " << checkpoint_name);
    return ok;
}

bool InferenceWorkerManager::restoreKvCache(const std::string& checkpoint_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    static std::mt19937_64 rng(std::random_device{}());
    std::ostringstream oss;
    oss << std::hex << rng();
    std::string command_id = "restorekv-" + oss.str().substr(0, 8);

    json cmd = InferenceProtocol::createRestoreKvCommand(checkpoint_name, command_id);
    bool ok = sendCommandAndWaitReady(cmd, 30);
    if (ok) LOG_INFO("[" << process_type_ << "Worker] KV restored: " << checkpoint_name);
    return ok;
}

// ─────────────────────────────────────────────────────────────────────────────
// terminateWorker — SIGKILL for cancellation (Section 7)
// ─────────────────────────────────────────────────────────────────────────────
void InferenceWorkerManager::terminateWorker(bool force) {
    // Join any pending background KV reset BEFORE acquiring mutex_.
    // The reset thread calls sendReset() which acquires mutex_, so holding
    // mutex_ while joining would deadlock. In the common case the reset
    // already completed, so this is a no-op join.
    // Failure to join here causes std::terminate() when the std::thread
    // destructor fires on a still-joinable thread during model eviction.
    waitForPendingReset();

    std::lock_guard<std::mutex> lock(mutex_);
    cleanupWorker(force);
}

bool InferenceWorkerManager::forceKillActiveWorker() {
    const int target_pid =
        watchdog_target_pid_.exchange(-1, std::memory_order_relaxed);
    if (target_pid <= 0) {
        LOG_WARN("[" << process_type_
                 << "Worker] Active worker force-kill skipped; no target PID");
        return false;
    }

    if (::kill(target_pid, 0) != 0) {
        LOG_WARN("[" << process_type_
                 << "Worker] Active worker force-kill skipped for PID "
                 << target_pid << ": " << strerror(errno));
        return false;
    }

    if (::kill(target_pid, SIGKILL) != 0) {
        LOG_WARN("[" << process_type_
                 << "Worker] Active worker SIGKILL failed for PID "
                 << target_pid << ": " << strerror(errno));
        return false;
    }

    LOG_WARN("[" << process_type_
             << "Worker] Active worker SIGKILL sent to PID " << target_pid);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// shutdown — Graceful shutdown
// ─────────────────────────────────────────────────────────────────────────────
void InferenceWorkerManager::shutdown() {
    // Join any pending background KV reset BEFORE acquiring mutex_.
    // Same reasoning as terminateWorker(): the reset thread holds mutex_
    // internally, so we must not hold it while joining.
    // This also covers the destructor path (~InferenceWorkerManager calls
    // shutdown()), ensuring reset_thread_ is never joinable at destruction.
    waitForPendingReset();

    std::lock_guard<std::mutex> lock(mutex_);
    if (worker_pid_ > 0 && sock_fd_ >= 0) {
        try {
            sendMessage(InferenceProtocol::createShutdownCommand());
        } catch (...) {}
    }
    cleanupWorker(false);
}

// ─────────────────────────────────────────────────────────────────────────────
// Accessors
// ─────────────────────────────────────────────────────────────────────────────
bool InferenceWorkerManager::isWorkerRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return isWorkerRunningLocked();
}

bool InferenceWorkerManager::isWorkerRunningLocked() const {
    if (worker_pid_ <= 0) return false;
    // kill(pid, 0) alone can't tell a live process apart from a zombie —
    // an exited-but-unreaped child still holds its PID, so kill() keeps
    // returning 0 until something waitpid()s it. Reap it here if it has
    // exited so a crashed worker is detected immediately instead of on
    // the next request's IPC write failure.
    int status = 0;
    pid_t r = ::waitpid(worker_pid_, &status, WNOHANG);
    return r == 0;
}

std::string InferenceWorkerManager::getCurrentModelId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_model_id_;
}

std::chrono::steady_clock::time_point InferenceWorkerManager::lastActivityTime() const {
    return last_activity_time_.load();
}

// ─────────────────────────────────────────────────────────────────────────────
// Watchdog — Section 4.F
// ─────────────────────────────────────────────────────────────────────────────

// watchdogThreadFunc — runs on the background watchdog thread.
//
// Design constraints (see header Section 4.F):
//   • NEVER acquires mutex_.  All state is read via atomics.
//   • Sends SIGKILL directly to watchdog_target_pid_ (no Layer-3 helpers).
//   • Clears watchdog_target_pid_ before sending SIGKILL to prevent a
//     second kill if cleanupWorker() races with the watchdog.
void InferenceWorkerManager::watchdogThreadFunc() {
    LOG_INFO("[" << process_type_ << "Watchdog] Started"
             << " active_timeout=" << active_timeout_seconds_ << "s"
             << " check_interval=" << watchdog_interval_seconds_ << "s");

    while (true) {
        // Sleep for the check interval, but wake immediately on stop signal.
        {
            std::unique_lock<std::mutex> cv_lock(watchdog_cv_mutex_);
            watchdog_cv_.wait_for(cv_lock,
                std::chrono::seconds(watchdog_interval_seconds_),
                [this] { return watchdog_stop_.load(std::memory_order_relaxed); });
        }

        if (watchdog_stop_.load(std::memory_order_relaxed)) break;

        // Read the target PID atomically.  -1 means no worker is running.
        int target_pid = watchdog_target_pid_.load(std::memory_order_relaxed);
        if (target_pid <= 0) continue;

        // Verify the process is still alive (cheap signal-0 probe).
        if (::kill(target_pid, 0) != 0) {
            // Process already gone — nothing to do.
            continue;
        }

        const bool active = is_active_.load(std::memory_order_acquire);
        if (!active) continue;

        // Check whether the last IPC activity is within the allowed window.
        auto last = last_activity_time_.load(std::memory_order_relaxed);
        if (last == std::chrono::steady_clock::time_point{}) {
            // Activity clock not yet set (worker still initialising).
            continue;
        }

        auto now     = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last).count();

        if (elapsed < active_timeout_seconds_) continue;

        // ── Timeout detected ──────────────────────────────────────────────
        LOG_WARN("[" << process_type_ << "Watchdog] Worker PID " << target_pid
                 << " unresponsive for " << elapsed << "s"
                 << " (threshold=" << active_timeout_seconds_ << "s)."
                 << " Sending SIGKILL.");

        // Clear the target PID *before* sending SIGKILL to prevent a
        // double-kill if cleanupWorker() runs concurrently.
        watchdog_target_pid_.store(-1, std::memory_order_relaxed);

        // Send SIGKILL directly — no mutex, no Layer-3 helpers.
        // The main thread's readMessage() will receive an EOF/error on the
        // socket and propagate it to the caller via on_error callback.
        // The next ensureWorkerRunning() call will spawn a fresh worker.
        if (::kill(target_pid, SIGKILL) == 0) {
            LOG_WARN("[" << process_type_ << "Watchdog] SIGKILL sent to PID "
                     << target_pid << ".");
        } else {
            LOG_WARN("[" << process_type_ << "Watchdog] SIGKILL failed for PID "
                     << target_pid << ": " << strerror(errno));
        }
    }

    LOG_INFO("[" << process_type_ << "Watchdog] Stopped.");
}

// startWatchdog — launch the watchdog background thread.
// Called from startWorker() while mutex_ is held.
void InferenceWorkerManager::startWatchdog() {
    // Stop any previously running watchdog first (e.g. after a model switch).
    stopWatchdog();

    watchdog_stop_.store(false, std::memory_order_relaxed);
    watchdog_thread_ = std::thread(&InferenceWorkerManager::watchdogThreadFunc, this);
}

// stopWatchdog — signal the watchdog to exit and join its thread.
// Called from cleanupWorker() while mutex_ is held.
// Safe because the watchdog thread never acquires mutex_.
void InferenceWorkerManager::stopWatchdog() {
    watchdog_stop_.store(true, std::memory_order_relaxed);
    watchdog_cv_.notify_all();
    if (watchdog_thread_.joinable()) {
        watchdog_thread_.join();
    }
}
