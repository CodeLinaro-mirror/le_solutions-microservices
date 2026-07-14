// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifdef QAI_FORGE_BUILD_LLAMACPP

#include "qai_forge/worker/LlamaCppWorkerManager.h"
#include "qai_forge/utils/Logger.h"

#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <cstring>
#include <sstream>
#include <chrono>
#include <thread>

namespace qai_forge {

LlamaCppWorkerManager::LlamaCppWorkerManager()
    : next_port_(BASE_PORT) {
    // Port allocation starts at BASE_PORT (50000) and increments dynamically
    // freed_ports_ will accumulate recycled ports for reuse
}

LlamaCppWorkerManager::~LlamaCppWorkerManager() {
    stopAllWorkers();
}

int LlamaCppWorkerManager::startWorker(const std::string& model_id,
                                        const std::string& model_path,
                                        const std::string& server_binary_path,
                                        int context_size) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if worker already exists for this model_id
    auto it = workers_.find(model_id);
    if (it != workers_.end()) {
        if (isProcessAlive(it->second.pid)) {
            LOG_INFO("[LlamaCppWorkerManager] Worker already running for model_id: "
                     + model_id + " on port " + std::to_string(it->second.port));
            return it->second.port;
        } else {
            // Process died, clean up
            LOG_WARN("[LlamaCppWorkerManager] Worker process died for model_id: "
                     + model_id + ", restarting");
            releasePort(it->second.port);
            workers_.erase(it);
        }
    }

    // Allocate port
    int port = allocatePort();
    if (port < 0) {
        LOG_ERROR("[LlamaCppWorkerManager] Failed to allocate port for llama-server worker");
        return -1;
    }

    // Fork and exec llama-server
    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("[LlamaCppWorkerManager] Failed to fork llama-server process: "
                  + std::string(strerror(errno)));
        releasePort(port);
        return -1;
    }

    if (pid == 0) {
        // Child process: exec llama-server
        std::string port_str = std::to_string(port);
        std::string ctx_str = std::to_string(context_size);

        // Redirect stdout/stderr to log file for debugging
        std::string log_path = "/tmp/llama-server-" + model_id + ".log";
        int logfd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logfd >= 0) {
            dup2(logfd, STDOUT_FILENO);
            dup2(logfd, STDERR_FILENO);
            close(logfd);
        }

        // Execute llama-server with OpenAI-compatible API
        // Dynamically inject the path to the server binary instead of hardcoding /usr/bin/
        execl(server_binary_path.c_str(),
              "llama-server",
              "-m", model_path.c_str(),
              "--host", "127.0.0.1",
              "--port", port_str.c_str(),
              "--ctx-size", ctx_str.c_str(),
              "--n-gpu-layers", "999",  // Offload all layers to NPU (Qualcomm Hexagon fallback)
              "--parallel", "1",  // Single request at a time
              nullptr);

        // If exec fails, exit child
        _exit(1);
    }

    // Parent process: wait for server to be ready
    LOG_INFO("[LlamaCppWorkerManager] Started llama-server worker for model_id: "
             + model_id + " (PID: " + std::to_string(pid) + ", port: "
             + std::to_string(port) + ")");

    // Wait up to 120 seconds for health check (model loading can take time)
    bool healthy = false;
    for (int i = 0; i < 240; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Check if process died during startup
        if (!isProcessAlive(pid)) {
            LOG_ERROR("[LlamaCppWorkerManager] llama-server process died during startup for model_id: "
                      + model_id + ". Check logs at /tmp/llama-server-" + model_id + ".log");
            releasePort(port);
            return -1;
        }

        if (checkHealth(port)) {
            healthy = true;
            break;
        }
    }

    if (!healthy) {
        LOG_ERROR("[LlamaCppWorkerManager] llama-server worker failed health check for model_id: "
                  + model_id + ", killing process");
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
        releasePort(port);
        return -1;
    }

    // Register worker
    WorkerProcess worker;
    worker.pid = pid;
    worker.port = port;
    worker.model_id = model_id;
    worker.model_path = model_path;
    worker.is_healthy = true;
    workers_[model_id] = worker;

    LOG_INFO("[LlamaCppWorkerManager] llama-server worker ready for model_id: " + model_id);
    return port;
}

bool LlamaCppWorkerManager::stopWorker(const std::string& model_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = workers_.find(model_id);
    if (it == workers_.end()) {
        return false;
    }

    pid_t pid = it->second.pid;
    int port = it->second.port;

    LOG_INFO("[LlamaCppWorkerManager] Stopping llama-server worker for model_id: "
             + model_id + " (PID: " + std::to_string(pid) + ")");

    // Send SIGTERM for graceful shutdown
    kill(pid, SIGTERM);

    // Wait up to 5 seconds for process to exit
    for (int i = 0; i < 10; ++i) {
        int status;
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            LOG_INFO("[LlamaCppWorkerManager] Worker process exited gracefully for model_id: "
                     + model_id);
            releasePort(port);
            workers_.erase(it);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // Force kill if still alive
    LOG_WARN("[LlamaCppWorkerManager] Worker did not exit gracefully for model_id: "
             + model_id + ", sending SIGKILL");
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    releasePort(port);
    workers_.erase(it);
    return true;
}

int LlamaCppWorkerManager::getWorkerPort(const std::string& model_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = workers_.find(model_id);
    return (it != workers_.end()) ? it->second.port : -1;
}

bool LlamaCppWorkerManager::isWorkerHealthy(const std::string& model_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = workers_.find(model_id);
    if (it == workers_.end()) {
        return false;
    }
    return isProcessAlive(it->second.pid) && checkHealth(it->second.port);
}

void LlamaCppWorkerManager::stopAllWorkers() {
    std::lock_guard<std::mutex> lock(mutex_);
    LOG_INFO("[LlamaCppWorkerManager] Stopping all llama-server workers");

    for (auto& [model_id, worker] : workers_) {
        kill(worker.pid, SIGTERM);
    }

    // Wait for all processes
    for (auto& [model_id, worker] : workers_) {
        waitpid(worker.pid, nullptr, 0);
        releasePort(worker.port);
    }

    workers_.clear();
}

int LlamaCppWorkerManager::allocatePort() {
    // Strategy: Reuse freed ports first (lowest port), then increment next_port_

    // Check if we have any freed ports to reuse
    if (!freed_ports_.empty()) {
        int port = *freed_ports_.begin();
        freed_ports_.erase(freed_ports_.begin());
        LOG_DEBUG("[LlamaCppWorkerManager] Allocated recycled port: " + std::to_string(port));
        return port;
    }

    // Otherwise, use next_port_ and increment
    int port = next_port_++;
    LOG_DEBUG("[LlamaCppWorkerManager] Allocated new port: " + std::to_string(port));
    return port;
}

void LlamaCppWorkerManager::releasePort(int port) {
    // Add port to freed_ports_ for reuse
    freed_ports_.insert(port);
    LOG_DEBUG("[LlamaCppWorkerManager] Released port for reuse: " + std::to_string(port));
}

bool LlamaCppWorkerManager::isProcessAlive(pid_t pid) const {
    return (kill(pid, 0) == 0);
}

bool LlamaCppWorkerManager::checkHealth(int port) const {
    // Create socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return false;
    }

    // Set timeout
    struct timeval timeout;
    timeout.tv_sec = HEALTH_CHECK_TIMEOUT_MS / 1000;
    timeout.tv_usec = (HEALTH_CHECK_TIMEOUT_MS % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // Connect to localhost:port
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return false;
    }

    // Send HTTP GET /health
    const char* request = "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
    if (send(sock, request, strlen(request), 0) < 0) {
        close(sock);
        return false;
    }

    // Read response
    char buffer[1024];
    ssize_t bytes = recv(sock, buffer, sizeof(buffer) - 1, 0);
    close(sock);

    if (bytes <= 0) {
        return false;
    }

    buffer[bytes] = '\0';
    // Check for "200 OK" in response
    return (strstr(buffer, "200 OK") != nullptr);
}

} // namespace qai_forge

#endif // QAI_FORGE_BUILD_LLAMACPP
