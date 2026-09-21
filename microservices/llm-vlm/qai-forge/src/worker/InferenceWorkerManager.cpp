// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// InferenceWorkerManager — Layer 3/4 Implementation
//
// Manages the genai-inference-worker subprocess lifecycle and communicates
// via JSON Lines over a Unix Domain Socket (socketpair). A dedicated reader
// thread demuxes responses by event_id/command_id so that up to max_slots_
// EXECUTE requests can be in flight concurrently under continuous batching;
// non-CB models (max_slots_ == 1) see identical externally-observable
// behavior to the original single-slot implementation.
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
constexpr size_t kDefaultPromptShmBytes = 4u * 1024 * 1024;
constexpr size_t kDefaultImageShmBytes  = 32u * 1024 * 1024;  // 32MB per slot

struct ActiveRequestGuard {
    explicit ActiveRequestGuard(std::atomic<int>& count) : count_(count) {
        count_.fetch_add(1, std::memory_order_acq_rel);
    }

    ~ActiveRequestGuard() {
        count_.fetch_sub(1, std::memory_order_acq_rel);
    }

    ActiveRequestGuard(const ActiveRequestGuard&) = delete;
    ActiveRequestGuard& operator=(const ActiveRequestGuard&) = delete;

private:
    std::atomic<int>& count_;
};
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────
InferenceWorkerManager::InferenceWorkerManager(const std::string& process_type)
    : process_type_(process_type) {
    auto read_env_int = [](const char* name, int default_val) -> int {
        const char* val = std::getenv(name);
        if (!val) return default_val;
        try { return std::stoi(val); } catch (...) { return default_val; }
    };
    active_timeout_seconds_ = read_env_int("GENAI_WORKER_ACTIVE_TIMEOUT", 600);
    if (active_timeout_seconds_ <= 0) {
        active_timeout_seconds_ = 600;
    }

    if (process_type_ == "vlm") {
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
// generateCommandId — thread_local rng avoids racing on shared state now that
// multiple request threads can generate command ids concurrently under a
// shared lock.
// ─────────────────────────────────────────────────────────────────────────────
std::string InferenceWorkerManager::generateCommandId(const std::string& prefix) {
    static thread_local std::mt19937_64 rng(
        std::hash<std::thread::id>{}(std::this_thread::get_id()) ^
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::ostringstream oss;
    oss << std::hex << rng();
    return prefix + "-" + oss.str().substr(0, 8);
}

// ─────────────────────────────────────────────────────────────────────────────
// ensureWorkerRunning
// ─────────────────────────────────────────────────────────────────────────────
void InferenceWorkerManager::ensureWorkerRunning(const std::string& model_id,
                                                  const std::string& config_file,
                                                  const std::string& sampler_config) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

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

void InferenceWorkerManager::setMaxSlots(int max_slots) {
    max_slots_ = std::clamp(max_slots, 1, 16);
}

// ─────────────────────────────────────────────────────────────────────────────
// startWorker — Fork/exec the genai-inference-worker binary
// ─────────────────────────────────────────────────────────────────────────────
void InferenceWorkerManager::startWorker(const std::string& model_id,
                                          const std::string& config_file,
                                          const std::string& sampler_config) {
    // max_slots_ is supplied by the backend; default remains one slot.
    const bool is_vlm_worker = (process_type_ == "vlm");

    // Prompt shared memory is retained only for the VLM worker, whose
    // protocol still carries prompt_ref alongside image_refs. LLM prompts
    // travel inline in the EXECUTE JSON command.
    int prompt_shm_fd = -1;
    void* prompt_shm_ptr = nullptr;
    if (is_vlm_worker) {
        prompt_shm_fd = memfd_create("qai-forge-prompt-shm", 0);
        if (prompt_shm_fd < 0) throw std::runtime_error(std::string("memfd_create failed: ") + strerror(errno));
        if (ftruncate(prompt_shm_fd, static_cast<off_t>(prompt_shm_bytes_)) < 0) {
            ::close(prompt_shm_fd); throw std::runtime_error(std::string("ftruncate failed: ") + strerror(errno));
        }
        prompt_shm_ptr = mmap(nullptr, prompt_shm_bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, prompt_shm_fd, 0);
        if (prompt_shm_ptr == MAP_FAILED) {
            ::close(prompt_shm_fd); throw std::runtime_error(std::string("mmap failed: ") + strerror(errno));
        }
    }

    // Create a socketpair for bidirectional IPC
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        if (prompt_shm_ptr) munmap(prompt_shm_ptr, prompt_shm_bytes_);
        if (prompt_shm_fd >= 0) ::close(prompt_shm_fd);
        throw std::runtime_error(std::string("socketpair() failed: ") + strerror(errno));
    }

    int parent_fd = sv[0];
    int child_fd  = sv[1];

    // Create the image shared-memory region (VLM only).

    int   image_shm_fd  = -1;
    void* image_shm_ptr = nullptr;
    if (is_vlm_worker) {
        image_shm_fd = memfd_create("qai-forge-image-shm", 0);
        if (image_shm_fd < 0) {
            if (prompt_shm_ptr) munmap(prompt_shm_ptr, prompt_shm_bytes_);
            if (prompt_shm_fd >= 0) ::close(prompt_shm_fd);
            ::close(sv[0]); ::close(sv[1]);
            throw std::runtime_error(std::string("memfd_create (image) failed: ") + strerror(errno));
        }
        if (ftruncate(image_shm_fd, static_cast<off_t>(image_shm_bytes_)) < 0) {
            ::close(image_shm_fd);
            if (prompt_shm_ptr) munmap(prompt_shm_ptr, prompt_shm_bytes_);
            if (prompt_shm_fd >= 0) ::close(prompt_shm_fd);
            ::close(sv[0]); ::close(sv[1]);
            throw std::runtime_error(std::string("ftruncate (image) failed: ") + strerror(errno));
        }
        image_shm_ptr = mmap(nullptr, image_shm_bytes_, PROT_READ | PROT_WRITE,
                             MAP_SHARED, image_shm_fd, 0);
        if (image_shm_ptr == MAP_FAILED) {
            ::close(image_shm_fd);
            if (prompt_shm_ptr) munmap(prompt_shm_ptr, prompt_shm_bytes_);
            if (prompt_shm_fd >= 0) ::close(prompt_shm_fd);
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
    std::string env_var = socket_fd_var;
    std::string prompt_shm_fd_var = "PROMPT_SHM_FD=" + std::to_string(prompt_shm_fd);
    std::string prompt_shm_bytes_var = "PROMPT_SHM_BYTES=" + std::to_string(prompt_shm_bytes_);
    std::string image_shm_fd_var     = "IMAGE_SHM_FD=" + std::to_string(image_shm_fd);
    std::string image_shm_bytes_var  = "IMAGE_SHM_BYTES=" + std::to_string(image_shm_bytes_);

    pid_t pid = ::fork();
    if (pid < 0) {
        if (is_vlm_worker) { munmap(image_shm_ptr, image_shm_bytes_); ::close(image_shm_fd); }
        if (prompt_shm_ptr) munmap(prompt_shm_ptr, prompt_shm_bytes_);
        if (prompt_shm_fd >= 0) ::close(prompt_shm_fd);
        ::close(sv[0]);
        ::close(sv[1]);
        throw std::runtime_error(std::string("fork() failed: ") + strerror(errno));
    }

    if (pid == 0) {
        // ── Child process ──────────────────────────────────────────────────
        ::close(parent_fd);

        // Set the socket FD environment variable.
        ::putenv(const_cast<char*>(env_var.c_str()));
        if (is_vlm_worker) {
            ::putenv(const_cast<char*>(prompt_shm_fd_var.c_str()));
            ::putenv(const_cast<char*>(prompt_shm_bytes_var.c_str()));
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
    if (is_vlm_worker) {
        prompt_shm_ptr_ = prompt_shm_ptr;
        prompt_shm_fd_ = prompt_shm_fd;
        image_shm_ptr_ = image_shm_ptr;
        image_shm_fd_  = image_shm_fd;
    }
    socket_path_ = generateSocketPath(model_id);

    LOG_INFO("[" << process_type_ << "Worker] Started PID " << worker_pid_
             << " for model " << model_id << " max_slots=" << max_slots_);

    // Wait for READY from worker (reader thread not started yet — safe to
    // read directly on this thread while holding the exclusive lock).
    json ready = readMessage(30);
    if (ready.value("type", "") != ResponseType::READY) {
        cleanupWorker(true);
        throw std::runtime_error("Worker failed to send READY after startup");
    }

    // Send INIT command
    json init_cmd = InferenceProtocol::createInitCommand(model_id, config_file, sampler_config, max_slots_);
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
    // and start the background threads (watchdog + reader).
    watchdog_target_pid_.store(worker_pid_);
    last_activity_time_.store(std::chrono::steady_clock::now());
    active_request_count_.store(0);
    startWatchdog();

    reader_stop_.store(false, std::memory_order_relaxed);
    reader_thread_ = std::thread(&InferenceWorkerManager::readerThreadFunc, this);

    LOG_INFO("[" << process_type_ << "Worker] Ready for model: " << model_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// cleanupWorker
// ─────────────────────────────────────────────────────────────────────────────
void InferenceWorkerManager::cleanupWorker(bool force) {
    // Disarm the watchdog first. The watchdog thread never acquires mutex_,
    // so joining it here (while mutex_ is held by our caller) is safe.
    stopWatchdog();
    watchdog_target_pid_.store(-1);

    // Stop the reader thread: signal intent, then shutdown() (not close()) the
    // socket to unblock its blocking read()/select() cleanly without
    // invalidating the fd number while the reader thread may still reference
    // it, then join.
    reader_stop_.store(true, std::memory_order_relaxed);
    if (sock_fd_ >= 0) {
        ::shutdown(sock_fd_, SHUT_RDWR);
    }
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
    failAllPending("worker terminated");
    joinAllResetThreads();

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

    max_slots_ = 1;
    prompt_shm_bytes_ = 0;
    image_shm_bytes_ = 0;

    read_buf_.clear();
    current_model_id_.clear();
    active_request_count_.store(0);
}

// ─────────────────────────────────────────────────────────────────────────────
// sendMessage — Write a JSON Line to the socket (thread-safe)
// ─────────────────────────────────────────────────────────────────────────────
void InferenceWorkerManager::sendMessage(const json& msg) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (sock_fd_ < 0) throw std::runtime_error("Worker socket not connected");
    std::string line = InferenceProtocol::serialize(msg);
    size_t offset = 0;
    while (offset < line.size()) {
        ssize_t written = ::write(sock_fd_, line.data() + offset, line.size() - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("Socket write failed: ") + strerror(errno));
        }
        if (written == 0) throw std::runtime_error("Socket write returned zero bytes");
        offset += static_cast<size_t>(written);
    }
    // Any outbound IPC traffic counts as activity — reset the watchdog timer.
    last_activity_time_.store(std::chrono::steady_clock::now());
}

// ─────────────────────────────────────────────────────────────────────────────
// readMessage — Read a JSON Line from the socket with timeout.
// Only called from the reader thread once startWorker() has launched it (or
// directly from startWorker()'s own thread during the initial handshake,
// before the reader thread exists).
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
// writePromptToShm / writeImagesToShm — VLM-only fixed shared-memory transport
// ─────────────────────────────────────────────────────────────────────────────
json InferenceWorkerManager::writePromptToShm(const std::string& prompt) {
    if (prompt.size() > prompt_shm_bytes_) {
        throw std::runtime_error(
            "Prompt exceeds shared-memory capacity (" +
            std::to_string(prompt_shm_bytes_) + " bytes)");
    }
    size_t offset = 0;
    if (!prompt.empty()) {
        std::memcpy(static_cast<uint8_t*>(prompt_shm_ptr_) + offset, prompt.data(), prompt.size());
    }
    return {{"offset", offset}, {"len", prompt.size()}};
}

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
// Reader thread — the sole reader of sock_fd_ once started. Demuxes every
// incoming message to whichever caller thread is waiting on its event_id
// (EXECUTE stream) or command_id (RESET/SAVE_KV/RESTORE_KV/ABORT).
// ─────────────────────────────────────────────────────────────────────────────
void InferenceWorkerManager::readerThreadFunc() {
    LOG_INFO("[" << process_type_ << "Worker] Reader thread started");

    while (!reader_stop_.load(std::memory_order_relaxed)) {
        json msg;
        try {
            // Poll on the watchdog cadence so reader_stop_ is noticed promptly
            // without busy-looping.
            msg = readMessage(watchdog_interval_seconds_);
        } catch (const std::exception& e) {
            std::string what = e.what();
            if (what.find("Timeout") != std::string::npos) {
                continue;
            }
            if (reader_stop_.load(std::memory_order_relaxed)) {
                break; // expected: shutdown() during cleanupWorker()
            }
            LOG_WARN("[" << process_type_ << "Worker] Reader thread: fatal I/O error: " << what);
            failAllPending(what);
            break;
        }

        std::string type       = msg.value("type", "");
        std::string event_id   = msg.value("event_id", "");
        std::string command_id = msg.value("command_id", "");

        if (!event_id.empty() &&
            (type == ResponseType::TOKEN || type == ResponseType::DONE)) {
            dispatchExecuteEvent(type, event_id, msg);
        } else if (type == ResponseType::ERROR && !event_id.empty()) {
            dispatchExecuteEvent(type, event_id, msg);
        } else if (!command_id.empty() &&
                   (type == ResponseType::READY || type == ResponseType::ERROR)) {
            dispatchCommandEvent(type, command_id, msg);
        } else {
            LOG_DEBUG("[" << process_type_ << "Worker] Reader thread: unmatched message type="
                      << type << " event_id=" << event_id << " command_id=" << command_id);
        }
    }

    LOG_INFO("[" << process_type_ << "Worker] Reader thread stopped");
}

void InferenceWorkerManager::dispatchExecuteEvent(const std::string& type,
                                                    const std::string& event_id,
                                                    const json& msg) {
    std::shared_ptr<PendingExecute> pending;
    {
        std::lock_guard<std::mutex> lock(pending_execute_mutex_);
        auto it = pending_execute_.find(event_id);
        if (it == pending_execute_.end()) return; // stale/unknown — drop
        pending = it->second;
        if (type != ResponseType::TOKEN) {
            pending_execute_.erase(it); // DONE/ERROR terminate the request
        }
    }

    if (type == ResponseType::TOKEN) {
        pending->on_token(InferenceProtocol::parseToken(msg));
    } else if (type == ResponseType::DONE) {
        const auto done = InferenceProtocol::parseDone(msg);
        LOG_INFO("[" << process_type_ << "Worker] INFERENCE_SUCCESS event="
                 << done.event_id << " session=" << done.session_id
                 << " finish_reason=" << done.finish_reason);
        pending->on_done(done);
        pending->done_promise.set_value();
    } else { // ERROR
        const auto error = InferenceProtocol::parseError(msg);
        LOG_ERROR("[" << process_type_ << "Worker] INFERENCE_ERROR event="
                  << error.event_id << " session=" << error.session_id
                  << " message=\"" << error.message << "\"");
        pending->on_error(error);
        pending->done_promise.set_value();
    }
}

void InferenceWorkerManager::dispatchCommandEvent(const std::string& type,
                                                    const std::string& command_id,
                                                    const json& msg) {
    std::shared_ptr<PendingCommand> pending;
    {
        std::lock_guard<std::mutex> lock(pending_command_mutex_);
        auto it = pending_command_.find(command_id);
        if (it == pending_command_.end()) return; // stale/unknown — drop
        pending = it->second;
        pending_command_.erase(it);
    }

    if (type == ResponseType::READY) {
        pending->promise.set_value(true);
    } else { // ERROR
        pending->error_message = msg.value("message", "Unknown error");
        pending->promise.set_value(false);
    }
}

void InferenceWorkerManager::failAllPending(const std::string& reason) {
    std::vector<std::shared_ptr<PendingExecute>> executes;
    {
        std::lock_guard<std::mutex> lock(pending_execute_mutex_);
        for (auto& entry : pending_execute_) executes.push_back(entry.second);
        pending_execute_.clear();
    }
    for (auto& p : executes) {
        try {
            p->on_error({"", "", "", std::string("Worker connection lost: ") + reason});
        } catch (...) {}
        p->done_promise.set_value();
    }

    std::vector<std::shared_ptr<PendingCommand>> commands;
    {
        std::lock_guard<std::mutex> lock(pending_command_mutex_);
        for (auto& entry : pending_command_) commands.push_back(entry.second);
        pending_command_.clear();
    }
    for (auto& p : commands) {
        p->error_message = reason;
        p->promise.set_value(false);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// sendCommandAndWait — register in pending_command_, send, block on future
// ─────────────────────────────────────────────────────────────────────────────
bool InferenceWorkerManager::sendCommandAndWait(const json& cmd, const std::string& command_id,
                                                 int timeout_seconds) {
    auto pending = std::make_shared<PendingCommand>();
    auto future = pending->promise.get_future();
    {
        std::lock_guard<std::mutex> lock(pending_command_mutex_);
        pending_command_[command_id] = pending;
    }

    try {
        sendMessage(cmd);
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(pending_command_mutex_);
        pending_command_.erase(command_id);
        LOG_ERROR("[" << process_type_ << "Worker] Failed to send command " << command_id
                  << ": " << e.what());
        return false;
    }

    if (future.wait_for(std::chrono::seconds(timeout_seconds)) == std::future_status::timeout) {
        std::lock_guard<std::mutex> lock(pending_command_mutex_);
        pending_command_.erase(command_id);
        LOG_ERROR("[" << process_type_ << "Worker] Command " << command_id << " timed out");
        return false;
    }

    bool ok = future.get();
    if (!ok) {
        LOG_ERROR("[" << process_type_ << "Worker] Command " << command_id
                  << " failed: " << pending->error_message);
    }
    return ok;
}

// ─────────────────────────────────────────────────────────────────────────────
// sendExecuteAndStream — protected helper
//
// Registers the EXECUTE request in pending_execute_, sends it, and blocks
// this caller thread until the reader thread signals DONE/ERROR (invoking
// on_token/on_done/on_error along the way). Caller MUST hold a shared lock
// on mutex_.
// ─────────────────────────────────────────────────────────────────────────────
void InferenceWorkerManager::sendExecuteAndStream(const json& execute_cmd,
                                                   const std::string& event_id,
                                                   TokenCallback on_token,
                                                   DoneCallback on_done,
                                                   ErrorCallback on_error) {
    const std::string session_id = execute_cmd.value("session_id", "");

    LOG_INFO("[" << process_type_ << "Worker] INFERENCE_START event=" << event_id
             << " session=" << session_id
             << " physical_genie_batch=unknown");

    auto pending = std::make_shared<PendingExecute>();
    pending->on_token = std::move(on_token);
    pending->on_done   = std::move(on_done);
    pending->on_error  = std::move(on_error);
    auto future = pending->done_promise.get_future();

    {
        std::lock_guard<std::mutex> lock(pending_execute_mutex_);
        pending_execute_[event_id] = pending;
    }

    try {
        sendMessage(execute_cmd);
    } catch (const std::exception& e) {
        {
            std::lock_guard<std::mutex> lock(pending_execute_mutex_);
            pending_execute_.erase(event_id);
        }
        pending->on_error({event_id, session_id, "", std::string("Socket error: ") + e.what()});
        return;
    }

    const auto timeout = std::chrono::seconds(
        active_timeout_seconds_ + 2 * watchdog_interval_seconds_);
    if (future.wait_for(timeout) == std::future_status::timeout) {
        {
            std::lock_guard<std::mutex> lock(pending_execute_mutex_);
            pending_execute_.erase(event_id);
        }
        pending->on_error({event_id, session_id, "", "Timeout waiting for worker response"});
        return;
    }
    // on_done/on_error were already invoked by the reader thread.
}

// ─────────────────────────────────────────────────────────────────────────────
// executeRequest
// ─────────────────────────────────────────────────────────────────────────────
void InferenceWorkerManager::executeRequest(const std::string& event_id,
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
                                             bool kv_invalidated) {
    waitForPendingReset(session_id);

    std::shared_lock<std::shared_mutex> lock(mutex_);

    LOG_DEBUG("[" << process_type_ << "Worker] executeRequest event_id=" << event_id
              << " session=" << session_id << " model=" << current_model_id_
              << " streaming=" << streaming << " max_tokens=" << max_tokens);

    ActiveRequestGuard active_guard{active_request_count_};

    json execute_cmd = InferenceProtocol::createLlmExecuteCommand(
        event_id, prompt, streaming, max_tokens, temperature,
        top_p, top_k, presence_penalty, frequency_penalty, bypass_think_filter,
        session_id, kv_invalidated);
    sendExecuteAndStream(execute_cmd, event_id, on_token, on_done, on_error);
}

void InferenceWorkerManager::executeStructuredRequest(
    const std::string& event_id,
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
    ErrorCallback on_error) {
    waitForPendingReset(session_id);

    std::shared_lock<std::shared_mutex> lock(mutex_);

    ActiveRequestGuard active_guard{active_request_count_};

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
        frequency_penalty,
        session_id);
    sendExecuteAndStream(execute_cmd, event_id, on_token, on_done, on_error);
}

// ─────────────────────────────────────────────────────────────────────────────
// sendReset — Called by Layer 2's ConcurrencyMiddleware (not by Layer 3 itself)
// ─────────────────────────────────────────────────────────────────────────────
void InferenceWorkerManager::sendReset(const std::string& session_id) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::string command_id = generateCommandId("reset");

    json reset_cmd = InferenceProtocol::createResetCommand(command_id, session_id);
    if (!sendCommandAndWait(reset_cmd, command_id, 10)) {
        throw std::runtime_error("RESET command failed or timed out");
    }
    LOG_INFO("[" << process_type_ << "Worker] KV cache reset successfully"
              << (session_id.empty() ? "" : (" (session=" + session_id + ")")));
}

void InferenceWorkerManager::sendReinitialize(const std::string& session_id) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const std::string command_id = generateCommandId("reinitialize");
    const json command =
        InferenceProtocol::createReinitializeCommand(command_id, session_id);
    if (!sendCommandAndWait(command, command_id, 30)) {
        throw std::runtime_error("REINITIALIZE command failed or timed out");
    }
    LOG_INFO("[" << process_type_ << "Worker] Lane reinitialized (session="
             << session_id << ")");
}

// ─────────────────────────────────────────────────────────────────────────────
// initiateBackgroundReset — Eager post-inference KV cache reset (per session)
//
// Spawns a background thread that calls sendReset(session_id). Returns
// immediately so the caller can return the response to the HTTP layer while
// the reset runs concurrently.
//
// The next executeRequest() call for this session_id waits on the shared
// completion via waitForPendingReset(session_id) before claiming a shm slot,
// ensuring clean KV state with minimal latency.
// ─────────────────────────────────────────────────────────────────────────────
void InferenceWorkerManager::initiateBackgroundReset(const std::string& session_id) {
    std::shared_ptr<PendingReset> previous;
    {
        std::lock_guard<std::mutex> lock(reset_threads_mutex_);
        auto it = reset_threads_.find(session_id);
        if (it != reset_threads_.end()) {
            previous = it->second;
            reset_threads_.erase(it);
        }
    }
    // Join outside the global map lock so unrelated sessions are not blocked.
    // Same-session ordering is handled by ModelRuntime.
    if (previous) {
        previous->completion.wait();
        std::lock_guard<std::mutex> worker_lock(previous->worker_mutex);
        if (previous->worker.joinable()) {
            previous->worker.join();
        }
    }

    auto pending = std::make_shared<PendingReset>();
    {
        std::lock_guard<std::mutex> lock(reset_threads_mutex_);
        reset_threads_[session_id] = pending;
        try {
            pending->worker = std::thread([this, session_id, pending]() {
                try {
                    sendReset(session_id);
                } catch (...) {
                    try {
                        LOG_WARN("[" << process_type_
                                 << "Worker] KV reset failed for session="
                                 << session_id
                                 << "; attempting lane reinitialization");
                        sendReinitialize(session_id);
                    } catch (...) {
                        LOG_ERROR("[" << process_type_
                                  << "Worker] Lane reinitialization failed for session="
                                  << session_id
                                  << "; worker recovery is required");
                        pending->completion_promise.set_exception(
                            std::current_exception());
                        return;
                    }
                }
                pending->completion_promise.set_value();
            });
        } catch (...) {
            reset_threads_.erase(session_id);
            pending->completion_promise.set_exception(std::current_exception());
            throw;
        }
    }
}

// waitForPendingReset — Block until the session's background reset completes
// ─────────────────────────────────────────────────────────────────────────────
void InferenceWorkerManager::waitForPendingReset(const std::string& session_id) {
    std::shared_ptr<PendingReset> pending;
    {
        std::lock_guard<std::mutex> lock(reset_threads_mutex_);
        auto it = reset_threads_.find(session_id);
        if (it == reset_threads_.end()) return;
        pending = it->second;
    }
    std::exception_ptr error;
    if (pending) {
        LOG_INFO("[" << process_type_ << "Worker] Waiting for background KV reset (session="
                 << session_id << ") to complete");
        try {
            pending->completion.get();
        } catch (...) {
            error = std::current_exception();
        }
        LOG_INFO("[" << process_type_ << "Worker] Background KV reset (session=" << session_id
                 << ") complete - ready for inference");
    }
    std::thread completed_worker;
    {
        std::lock_guard<std::mutex> lock(reset_threads_mutex_);
        auto pending_it = reset_threads_.find(session_id);
        if (pending_it != reset_threads_.end() && pending_it->second == pending &&
            pending->completion.wait_for(std::chrono::seconds(0)) ==
                std::future_status::ready) {
            std::lock_guard<std::mutex> worker_lock(pending->worker_mutex);
            completed_worker = std::move(pending->worker);
            reset_threads_.erase(pending_it);
        }
    }
    if (completed_worker.joinable()) completed_worker.join();
    if (error) {
        std::rethrow_exception(error);
    }
}

void InferenceWorkerManager::waitForSessionReady(
    const std::string& session_id) {
    waitForPendingReset(session_id);
}

// joinAllResetThreads — used during worker teardown, where every session's
// reset must be drained (not just one).
// ─────────────────────────────────────────────────────────────────────────────
void InferenceWorkerManager::joinAllResetThreads() {
    std::vector<std::shared_ptr<PendingReset>> pending_resets;
    {
        std::lock_guard<std::mutex> lock(reset_threads_mutex_);
        for (auto& entry : reset_threads_) {
            pending_resets.push_back(entry.second);
        }
        reset_threads_.clear();
    }
    for (const auto& pending : pending_resets) {
        try {
            pending->completion.wait();
        } catch (...) {
            // Teardown still must join the worker even when reset failed.
        }
        std::lock_guard<std::mutex> worker_lock(pending->worker_mutex);
        if (pending->worker.joinable()) {
            pending->worker.join();
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// saveKvCache / restoreKvCache — Section 6 (KV Cache Management)
// ─────────────────────────────────────────────────────────────────────────────
bool InferenceWorkerManager::saveKvCache(const std::string& checkpoint_name,
                                          const std::string& session_id) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::string command_id = generateCommandId("savekv");

    json cmd = InferenceProtocol::createSaveKvCommand(checkpoint_name, command_id, session_id);
    bool ok = sendCommandAndWait(cmd, command_id, 30);
    if (ok) LOG_INFO("[" << process_type_ << "Worker] KV saved: " << checkpoint_name);
    return ok;
}

bool InferenceWorkerManager::restoreKvCache(const std::string& checkpoint_name,
                                             const std::string& session_id) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::string command_id = generateCommandId("restorekv");

    json cmd = InferenceProtocol::createRestoreKvCommand(checkpoint_name, command_id, session_id);
    bool ok = sendCommandAndWait(cmd, command_id, 30);
    if (ok) LOG_INFO("[" << process_type_ << "Worker] KV restored: " << checkpoint_name);
    return ok;
}

void InferenceWorkerManager::sendClearSession(const std::string& session_id) {
    if (session_id.empty()) return;
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (!isWorkerRunningLocked()) return;
    const std::string command_id = generateCommandId("clear");
    json cmd = InferenceProtocol::createClearSessionCommand(session_id, command_id);
    if (!sendCommandAndWait(cmd, command_id, 10)) {
        LOG_WARN("[" << process_type_ << "Worker] clearSession failed: "
                 << session_id);
        return;
    }
    LOG_INFO("[" << process_type_ << "Worker] clearSession sent: " << session_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// sendAbort — session-scoped cancellation, primary path under continuous
// batching (whole-worker SIGKILL affects every concurrent session's engine).
// ─────────────────────────────────────────────────────────────────────────────
bool InferenceWorkerManager::sendAbort(const std::string& session_id) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (sock_fd_ < 0) return false;
    std::string command_id = generateCommandId("abort");

    json cmd = InferenceProtocol::createAbortCommand(session_id, command_id);
    bool ok = sendCommandAndWait(cmd, command_id, 10);
    if (ok) {
        LOG_INFO("[" << process_type_ << "Worker] ABORT sent for session=" << session_id);
    } else {
        LOG_WARN("[" << process_type_ << "Worker] ABORT failed or timed out for session="
                 << session_id);
    }
    return ok;
}

// ─────────────────────────────────────────────────────────────────────────────
// terminateWorker — SIGKILL for whole-worker cancellation (Section 7)
// ─────────────────────────────────────────────────────────────────────────────
void InferenceWorkerManager::terminateWorker(bool force) {
    // Join any pending background KV resets BEFORE acquiring the exclusive
    // lock. Reset threads call sendReset() which takes a shared lock, so
    // holding the exclusive lock while joining would deadlock.
    // Failure to join here causes std::terminate() when a std::thread
    // destructor fires on a still-joinable thread during model eviction.
    joinAllResetThreads();

    // Under continuous batching, other sessions may be holding a shared
    // lock inside a blocking EXECUTE wait (up to active_timeout_seconds_).
    // A forced teardown must not queue behind them for the exclusive lock —
    // kill the worker PID directly first (same no-lock pattern as
    // forceKillActiveWorker()); the resulting socket EOF makes the reader
    // thread fail every pending request via failAllPending(), which
    // releases their shared locks so the exclusive lock below is granted
    // promptly instead of after minutes of waiting.
    if (force) {
        forceKillActiveWorker();
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
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
    // Join any pending background KV resets BEFORE acquiring the exclusive
    // lock. Same reasoning as terminateWorker(): reset threads take a shared
    // lock internally, so we must not hold the exclusive lock while joining.
    // This also covers the destructor path (~InferenceWorkerManager calls
    // shutdown()), ensuring no reset thread is ever joinable at destruction.
    joinAllResetThreads();

    std::unique_lock<std::shared_mutex> lock(mutex_);
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
    std::shared_lock<std::shared_mutex> lock(mutex_);
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
    std::shared_lock<std::shared_mutex> lock(mutex_);
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

        const bool active = active_request_count_.load(std::memory_order_acquire) > 0;
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
        // The reader thread will receive an EOF/error on the socket and
        // propagate it to every pending request via failAllPending().
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
// Called from startWorker() while the exclusive lock is held.
void InferenceWorkerManager::startWatchdog() {
    // Stop any previously running watchdog first (e.g. after a model switch).
    stopWatchdog();

    watchdog_stop_.store(false, std::memory_order_relaxed);
    watchdog_thread_ = std::thread(&InferenceWorkerManager::watchdogThreadFunc, this);
}

// stopWatchdog — signal the watchdog to exit and join its thread.
// Called from cleanupWorker() while the exclusive lock is held.
// Safe because the watchdog thread never acquires mutex_.
void InferenceWorkerManager::stopWatchdog() {
    watchdog_stop_.store(true, std::memory_order_relaxed);
    watchdog_cv_.notify_all();
    if (watchdog_thread_.joinable()) {
        watchdog_thread_.join();
    }
}
