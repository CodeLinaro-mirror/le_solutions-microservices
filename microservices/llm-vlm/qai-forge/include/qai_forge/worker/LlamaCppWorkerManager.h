// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#ifdef QAI_FORGE_BUILD_LLAMACPP

#include <string>
#include <memory>
#include <unordered_map>
#include <set>
#include <mutex>
#include <sys/types.h>

namespace qai_forge {

/**
 * @brief Manages llama-server worker processes for GGUF models
 *
 * Each model gets its own isolated llama-server subprocess running on a unique port.
 * The worker manager handles:
 * - Process lifecycle (fork/exec, graceful shutdown, SIGTERM)
 * - Port allocation (starting at 50000, dynamically allocated)
 * - Health monitoring via /health endpoint
 * - Process reaping to prevent zombies
 * - Model ID → port mapping for fast lookup
 */
class LlamaCppWorkerManager {
public:
    struct WorkerProcess {
        pid_t pid;
        int port;
        std::string model_id;    // External identifier (e.g. "llama-3-8b-q4")
        std::string model_path;  // Path to .gguf file
        bool is_healthy;
    };

    LlamaCppWorkerManager();
    ~LlamaCppWorkerManager();

    /**
     * @brief Start a llama-server worker for the given model
     * @param model_id External model identifier (used for lookups)
     * @param model_path Absolute path to .gguf file
     * @param server_binary_path Absolute path to the llama-server binary
     * @param context_size Context window size (default: 2048)
     * @return Port number on success, -1 on failure
     */
    int startWorker(const std::string& model_id,
                    const std::string& model_path,
                    const std::string& server_binary_path,
                    int context_size = 2048);

    /**
     * @brief Stop a worker process gracefully
     * @param model_id Model ID identifying the worker
     * @return true if stopped successfully
     */
    bool stopWorker(const std::string& model_id);

    /**
     * @brief Get the port for a running worker
     * @param model_id Model ID identifying the worker
     * @return Port number, or -1 if worker not found
     */
    int getWorkerPort(const std::string& model_id) const;

    /**
     * @brief Check if a worker is healthy
     * @param model_id Model ID identifying the worker
     * @return true if worker is running and healthy
     */
    bool isWorkerHealthy(const std::string& model_id) const;

    /**
     * @brief Stop all workers (called on shutdown)
     */
    void stopAllWorkers();

private:
    /**
     * @brief Allocate an available port starting from BASE_PORT
     * Uses freed_ports_ first (recycled), then next_port_ (monotonic)
     * @return Port number, or -1 if allocation fails
     */
    int allocatePort();

    /**
     * @brief Release a port back to the freed pool for reuse
     * @param port Port to release
     */
    void releasePort(int port);

    /**
     * @brief Check if a worker process is still alive
     * @param pid Process ID to check
     * @return true if process exists
     */
    bool isProcessAlive(pid_t pid) const;

    /**
     * @brief Perform HTTP GET request to check /health endpoint
     * @param port Port to check
     * @return true if health check succeeds
     */
    bool checkHealth(int port) const;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, WorkerProcess> workers_;  // model_id → WorkerProcess
    std::set<int> freed_ports_;  // Recycled ports (reused before incrementing next_port_)
    int next_port_;              // Next port to allocate (monotonically increasing)

    static constexpr int BASE_PORT = 50000;
    static constexpr int HEALTH_CHECK_TIMEOUT_MS = 5000;
};

} // namespace qai_forge

#endif // QAI_FORGE_BUILD_LLAMACPP
