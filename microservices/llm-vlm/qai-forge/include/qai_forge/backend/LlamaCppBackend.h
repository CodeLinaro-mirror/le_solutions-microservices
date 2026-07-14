// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#ifdef QAI_FORGE_BUILD_LLAMACPP

#include "qai_forge/backend/IGenerativeBackend.h"
#include "qai_forge/worker/LlamaCppWorkerManager.h"
#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <functional>

using json = nlohmann::ordered_json;

namespace qai_forge {

/**
 * @brief Backend implementation for llama.cpp with Hexagon NPU support
 *
 * This backend communicates with llama-server subprocesses via HTTP.
 * The LlamaCppOrchestrator calls httpPostBlocking() or httpPostStreaming()
 * to send OpenAI-formatted JSON to /v1/chat/completions endpoint.
 *
 * Design:
 * - Backend manages llama-server subprocess lifecycle
 * - Backend provides HTTP communication primitives
 * - Orchestrator builds OpenAI JSON and calls HTTP methods
 * - Backend is stateless (no stored orchestrator or session references)
 */
class LlamaCppBackend : public IGenerativeBackend {
public:
    explicit LlamaCppBackend(const std::string& server_binary_path);
    ~LlamaCppBackend() override;

    // IGenerativeBackend interface
    std::string name() const override { return "LlamaCpp"; }
    BackendCapabilities capabilities() const override;

    void loadModel(const std::string& model_id) override;
    void unloadModel(bool force = false) override;

    void ensureWorkerRunning(const std::string& model_id,
                            const std::string& config_file,
                            const std::string& sampler_file) override;

    void generate(
        const std::string& event_id,
        const std::string& prompt,
        bool               streaming,
        int                max_tokens,
        float              temperature,
        float              top_p,
        int                top_k,
        float              presence_penalty,
        float              frequency_penalty,
        bool               use_reasoning,
        std::function<void(const IPCTokenEvent&)>  on_token,
        std::function<void(const IPCDoneEvent&)>   on_done,
        std::function<void(const IPCErrorEvent&)>  on_error) override;

    void terminateWorker(bool force = false) override;
    bool isHealthy() const override;

    /**
     * @brief Get the HTTP port for this model's llama-server worker
     * @return Port number, or -1 if worker not started
     */
    int getWorkerPort() const;

    /**
     * @brief Perform blocking HTTP POST to llama-server
     * Called by LlamaCppOrchestrator for non-streaming requests.
     * @param endpoint HTTP endpoint (e.g., "/v1/chat/completions")
     * @param body JSON request body
     * @return JSON response body
     */
    json httpPostBlocking(const std::string& endpoint, const json& body);

    /**
     * @brief Perform streaming HTTP POST with SSE parsing
     * Called by LlamaCppOrchestrator for streaming requests.
     * @param endpoint HTTP endpoint
     * @param body JSON request body
     * @param on_chunk Callback for each SSE chunk (raw SSE data: "data: {...}\n\n")
     */
    void httpPostStreaming(const std::string& endpoint, const json& body,
                          std::function<void(const std::string&)> on_chunk);

private:
    std::string server_binary_path_;
    std::string model_id_;    // External model identifier (used for worker lookup)
    std::string model_path_;  // File system path to .gguf file
    std::shared_ptr<LlamaCppWorkerManager> worker_manager_;
    int worker_port_;
    bool initialized_;

    /**
     * @brief Create a TCP socket and connect to localhost:port
     * @return Socket file descriptor, or -1 on failure
     */
    int createSocket(const std::string& host, int port);

    /**
     * @brief Send HTTP request to socket
     */
    void sendHttpRequest(int sockfd, const std::string& method,
                        const std::string& path, const std::string& body);

    /**
     * @brief Receive complete HTTP response (blocking mode)
     * @return Response body (JSON string)
     */
    std::string receiveHttpResponse(int sockfd);

    /**
     * @brief Receive streaming HTTP response with SSE parsing
     * Calls on_chunk for each "data: {...}\n\n" event
     */
    void receiveHttpStreamingResponse(int sockfd,
                                     std::function<void(const std::string&)> on_chunk);

    /**
     * @brief Wait for llama-server to become ready
     * Polls /health endpoint until success or timeout
     */
    bool waitForServerReady(int timeout_seconds = 30);

    /**
     * @brief Internal HTTP POST (blocking) - returns raw string
     */
    std::string httpPostBlockingInternal(const std::string& endpoint, const std::string& body);

    /**
     * @brief Internal HTTP POST (streaming) - raw string body
     */
    void httpPostStreamingInternal(const std::string& endpoint, const std::string& body,
                                   std::function<void(const std::string&)> on_chunk);
};

} // namespace qai_forge

#endif // QAI_FORGE_BUILD_LLAMACPP
