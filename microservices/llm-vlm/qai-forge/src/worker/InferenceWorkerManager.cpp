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

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────
InferenceWorkerManager::InferenceWorkerManager(const std::string& process_type)
    : process_type_(process_type) {
    // Read watchdog timeout configuration from environment variables.
    // These can be tuned per deployment without recompilation.
    auto read_env_int = [](const char* name, int default_val) -> int {
        const char* val = std::getenv(name);
        if (!val) return default_val;
        try { return std::stoi(val); } catch (...) { return default_val; }
    };
    idle_timeout_seconds_      = read_env_int("GENAI_WORKER_IDLE_TIMEOUT",     120);
    active_timeout_seconds_    = read_env_int("GENAI_WORKER_ACTIVE_TIMEOUT",   600);
    watchdog_interval_seconds_ = read_env_int("GENAI_WATCHDOG_CHECK_INTERVAL", 5);
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
    if (worker_pid_ <= 0 || ::kill(worker_pid_, 0) != 0) {
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

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(sv[0]);
        ::close(sv[1]);
        throw std::runtime_error(std::string("fork() failed: ") + strerror(errno));
    }

    if (pid == 0) {
        // ── Child process ──────────────────────────────────────────────────
        ::close(parent_fd);

        // Set socket FD environment variable
        ::putenv(const_cast<char*>(env_var.c_str()));

        // Exec the worker binary
        ::execl(worker_bin, worker_bin, nullptr);

        // If exec fails — use write() directly; Logger mutex may be locked in parent
        const char msg[] = "[Worker] execl failed\n";
        ::write(STDERR_FILENO, msg, sizeof(msg) - 1);
        ::_exit(1);
    }

    // ── Parent process ─────────────────────────────────────────────────────
    ::close(child_fd);
    worker_pid_ = pid;
    sock_fd_ = parent_fd;
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
            if (::kill(worker_pid_, 0) == 0) {
                ::kill(worker_pid_, SIGKILL);
            }
        }
        ::waitpid(worker_pid_, nullptr, 0);
        worker_pid_ = -1;
    }

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

    // Use select() for timeout
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(sock_fd_, &read_fds);
    struct timeval tv{timeout_seconds, 0};

    int ready = ::select(sock_fd_ + 1, &read_fds, nullptr, nullptr, &tv);
    if (ready == 0) throw std::runtime_error("Timeout waiting for worker response");
    if (ready < 0) throw std::runtime_error(std::string("select() failed: ") + strerror(errno));

    // Read until newline
    std::string line;
    char c;
    while (::read(sock_fd_, &c, 1) == 1) {
        if (c == '\n') break;
        line += c;
    }

    if (line.empty()) throw std::runtime_error("Worker socket closed (EOF)");
    // Any inbound IPC traffic counts as activity — reset the watchdog timer.
    last_activity_time_.store(std::chrono::steady_clock::now());
    return InferenceProtocol::deserialize(line);
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
// an augmented command (with image_urls) and call this method directly,
// without duplicating the streaming loop.
// ─────────────────────────────────────────────────────────────────────────────
void InferenceWorkerManager::sendExecuteAndStream(const json& execute_cmd,
                                                   const std::string& event_id,
                                                   TokenCallback on_token,
                                                   DoneCallback on_done,
                                                   ErrorCallback on_error) {
    sendMessage(execute_cmd);

    bool pending_error = false;
    std::string pending_error_msg;

    while (true) {
        json response;
        try {
            response = readMessage(300); // 5 min timeout for long inference
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
    is_active_ = true;

    LOG_DEBUG("[" << process_type_ << "Worker] executeRequest event_id=" << event_id
              << " model=" << current_model_id_ << " streaming=" << streaming
              << " max_tokens=" << max_tokens);

    json execute_cmd = InferenceProtocol::createExecuteCommand(
        event_id, prompt, streaming, max_tokens, temperature,
        top_p, top_k, presence_penalty, frequency_penalty, bypass_think_filter
    );
    sendExecuteAndStream(execute_cmd, event_id, on_token, on_done, on_error);

    is_active_ = false;
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
    return worker_pid_ > 0 && ::kill(worker_pid_, 0) == 0;
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
             << " idle_timeout=" << idle_timeout_seconds_ << "s"
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

        // Check whether the last IPC activity is within the allowed window.
        auto last = last_activity_time_.load(std::memory_order_relaxed);
        if (last == std::chrono::steady_clock::time_point{}) {
            // Activity clock not yet set (worker still initialising).
            continue;
        }

        auto now     = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last).count();

        // Use a longer threshold while an EXECUTE is in flight (long inference).
        bool active   = is_active_.load(std::memory_order_relaxed);
        int threshold = active ? active_timeout_seconds_ : idle_timeout_seconds_;

        if (elapsed < threshold) continue;

        // ── Timeout detected ──────────────────────────────────────────────
        LOG_WARN("[" << process_type_ << "Watchdog] Worker PID " << target_pid
                 << " unresponsive for " << elapsed << "s"
                 << " (threshold=" << threshold << "s"
                 << ", active=" << (active ? "true" : "false") << ")."
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
