// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifdef QAI_FORGE_BUILD_LLAMACPP

#include "qai_forge/backend/LlamaCppBackend.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/Logger.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <chrono>

namespace qai_forge {

// Shared worker manager instance (singleton pattern)
static std::shared_ptr<LlamaCppWorkerManager> g_worker_manager = nullptr;
static std::mutex g_worker_manager_mutex;

LlamaCppBackend::LlamaCppBackend(const std::string& server_binary_path)
    : server_binary_path_(server_binary_path)
    , worker_port_(-1)
    , initialized_(false) {

    // Initialize shared worker manager if needed
    std::lock_guard<std::mutex> lock(g_worker_manager_mutex);
    if (!g_worker_manager) {
        g_worker_manager = std::make_shared<LlamaCppWorkerManager>();
    }
    worker_manager_ = g_worker_manager;
}

LlamaCppBackend::~LlamaCppBackend() {
    if (initialized_) {
        unloadModel(false);
    }
}

BackendCapabilities LlamaCppBackend::capabilities() const {
    BackendCapabilities caps;
    caps.context_strategy = ContextStrategy::FULL_RECOMPUTE;  // llama-server recomputes each turn
    caps.concurrency_model = ConcurrencyModel::EXCLUSIVE;  // One request at a time per worker
    caps.backend_filters_think_tokens = false;  // No internal think filtering
    caps.supports_kv_save_restore = false;  // Not supported
    return caps;
}

void LlamaCppBackend::loadModel(const std::string& model_id) {
    if (initialized_) {
        LOG_INFO("[LlamaCppBackend] Model already loaded");
        return;
    }

    model_id_ = model_id;

    // Look up the actual .gguf file path from ModelConfigManager
    model_path_ = ModelConfigManager::getInstance().getConfigFilePath(model_id);
    if (model_path_.empty()) {
        throw std::runtime_error("[LlamaCppBackend] No file path found for model: " + model_id);
    }

    LOG_INFO("[LlamaCppBackend] Loading model: " + model_id + " from " + model_path_);

    // Start llama-server worker (pass model_id, model_path, and binary_path)
    worker_port_ = worker_manager_->startWorker(model_id_, model_path_, server_binary_path_, 4096);
    if (worker_port_ < 0) {
        throw std::runtime_error("Failed to start llama-server worker");
    }

    initialized_ = true;
    LOG_INFO("[LlamaCppBackend] Model loaded successfully on port " + std::to_string(worker_port_));
}

void LlamaCppBackend::unloadModel(bool force) {
    if (!initialized_) {
        return;
    }

    LOG_INFO("[LlamaCppBackend] Unloading model");
    worker_manager_->stopWorker(model_id_);
    initialized_ = false;
    worker_port_ = -1;
    model_id_.clear();
    model_path_.clear();
}

void LlamaCppBackend::ensureWorkerRunning(const std::string& model_id,
                                         const std::string& config_file,
                                         const std::string& sampler_file) {
    // llama.cpp backend doesn't use config/sampler files
    (void)config_file;
    (void)sampler_file;

    if (!initialized_) {
        loadModel(model_id);
    }
}

void LlamaCppBackend::generate(
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
    std::function<void(const IPCErrorEvent&)>  on_error) {

    // This method should not be used for llama.cpp
    // The orchestrator should use httpPostBlocking/Streaming directly
    (void)event_id; (void)prompt; (void)streaming; (void)max_tokens;
    (void)temperature; (void)top_p; (void)top_k; (void)presence_penalty;
    (void)frequency_penalty; (void)use_reasoning;
    (void)on_token; (void)on_done;

    LOG_ERROR("[LlamaCppBackend] generate() called - use LlamaCppOrchestrator instead");
    IPCErrorEvent error_event;
    error_event.event_id = event_id;
    error_event.message = "LlamaCppBackend::generate() not supported - use httpPostBlocking/Streaming";
    on_error(error_event);
}

void LlamaCppBackend::terminateWorker(bool force) {
    unloadModel(force);
}

bool LlamaCppBackend::isHealthy() const {
    return initialized_ && worker_manager_->isWorkerHealthy(model_id_);
}

int LlamaCppBackend::getWorkerPort() const {
    return worker_port_;
}

json LlamaCppBackend::httpPostBlocking(const std::string& endpoint, const json& body) {
    if (!initialized_) {
        throw std::runtime_error("LlamaCppBackend not initialized");
    }

    std::string body_str = body.dump();
    std::string response_str = httpPostBlockingInternal(endpoint, body_str);

    try {
        return json::parse(response_str);
    } catch (const json::exception& e) {
        LOG_ERROR("[LlamaCppBackend] Failed to parse JSON response: " + std::string(e.what()));
        throw std::runtime_error("Invalid JSON response from llama-server");
    }
}

void LlamaCppBackend::httpPostStreaming(const std::string& endpoint, const json& body,
                                       std::function<void(const std::string&)> on_chunk) {
    if (!initialized_) {
        throw std::runtime_error("LlamaCppBackend not initialized");
    }

    std::string body_str = body.dump();
    httpPostStreamingInternal(endpoint, body_str, on_chunk);
}

int LlamaCppBackend::createSocket(const std::string& host, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        throw std::runtime_error("Failed to create socket");
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        throw std::runtime_error("Failed to connect to " + host + ":" + std::to_string(port));
    }

    return sock;
}

void LlamaCppBackend::sendHttpRequest(int sockfd, const std::string& method,
                                     const std::string& path, const std::string& body) {
    std::ostringstream request;
    request << method << " " << path << " HTTP/1.1\r\n"
            << "Host: localhost\r\n"
            << "Content-Type: application/json\r\n"
            << "Content-Length: " << body.size() << "\r\n";

    if (method == "POST" && path.find("/v1/chat/completions") != std::string::npos) {
        // Check if streaming is requested in the body
        if (body.find("\"stream\":true") != std::string::npos ||
            body.find("\"stream\": true") != std::string::npos) {
            request << "Accept: text/event-stream\r\n";
        }
    }

    request << "\r\n" << body;

    std::string req_str = request.str();
    if (send(sockfd, req_str.c_str(), req_str.size(), 0) < 0) {
        throw std::runtime_error("Failed to send HTTP request");
    }
}

std::string LlamaCppBackend::receiveHttpResponse(int sockfd) {
    std::string response;
    char buffer[4096];
    ssize_t bytes;

    while ((bytes = recv(sockfd, buffer, sizeof(buffer), 0)) > 0) {
        response.append(buffer, bytes);
    }

    // Extract body from HTTP response
    size_t body_start = response.find("\r\n\r\n");
    if (body_start == std::string::npos) {
        throw std::runtime_error("Invalid HTTP response - no body separator");
    }

    return response.substr(body_start + 4);
}

void LlamaCppBackend::receiveHttpStreamingResponse(int sockfd,
                                                  std::function<void(const std::string&)> on_chunk) {
    std::string buffer;
    char chunk[1024];
    bool headers_done = false;

    while (true) {
        ssize_t bytes = recv(sockfd, chunk, sizeof(chunk), 0);
        if (bytes <= 0) {
            break;
        }

        buffer.append(chunk, bytes);

        // Skip HTTP headers
        if (!headers_done) {
            size_t header_end = buffer.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                buffer = buffer.substr(header_end + 4);
                headers_done = true;
            } else {
                continue;
            }
        }

        // Process SSE events (data: {...}\n\n format)
        size_t pos = 0;
        while ((pos = buffer.find("\n\n")) != std::string::npos) {
            std::string event = buffer.substr(0, pos);
            buffer = buffer.substr(pos + 2);

            // Pass raw SSE chunk to callback (orchestrator will parse)
            if (!event.empty() && on_chunk) {
                on_chunk(event + "\n\n");
            }

            // Check for [DONE] marker
            if (event.find("data: [DONE]") != std::string::npos) {
                return;
            }
        }
    }
}

bool LlamaCppBackend::waitForServerReady(int timeout_seconds) {
    for (int i = 0; i < timeout_seconds * 2; ++i) {
        if (worker_manager_->isWorkerHealthy(model_id_)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return false;
}

std::string LlamaCppBackend::httpPostBlockingInternal(const std::string& endpoint,
                                                      const std::string& body) {
    int sock = createSocket("127.0.0.1", worker_port_);
    sendHttpRequest(sock, "POST", endpoint, body);
    std::string response = receiveHttpResponse(sock);
    close(sock);
    return response;
}

void LlamaCppBackend::httpPostStreamingInternal(const std::string& endpoint,
                                               const std::string& body,
                                               std::function<void(const std::string&)> on_chunk) {
    int sock = createSocket("127.0.0.1", worker_port_);
    sendHttpRequest(sock, "POST", endpoint, body);
    receiveHttpStreamingResponse(sock, on_chunk);
    close(sock);
}

} // namespace qai_forge

#endif // QAI_FORGE_BUILD_LLAMACPP
