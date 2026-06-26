//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <chrono>

namespace embedding {

/**
 * @brief Circuit-breaker-enabled HTTP client for the external embedding service.
 * Prevents cascading timeouts and failure loops in RAG systems.
 */
class EmbeddingClient {
public:
    EmbeddingClient(
        const std::string& base_url,
        const std::string& model,
        int32_t timeout_ms = 10000,
        int32_t failure_threshold = 5,
        int32_t cooldown_s = 30
    );

    ~EmbeddingClient();

    /**
     * @brief Computes embedding for a single text chunk.
     * @throws std::runtime_error if circuit is open or HTTP request fails.
     */
    std::vector<float> embed(const std::string& text);

    /**
     * @brief Computes embeddings for multiple text chunks in a single batch call.
     * @throws std::runtime_error if circuit is open or HTTP request fails.
     */
    std::vector<std::vector<float>> embedBatch(const std::vector<std::string>& texts);

    bool isCircuitOpen();
    int32_t consecutiveFailures() const;

private:
    std::string base_url_;
    std::string model_;
    int32_t timeout_ms_;
    int32_t failure_threshold_;
    std::chrono::seconds cooldown_s_;

    // File-based Embeddings Source of Truth
    bool use_file_ = false;
    std::string file_path_;
    std::vector<std::vector<float>> file_embeddings_;

    // Circuit Breaker State
    enum class CircuitState { Closed, Open, HalfOpen };
    CircuitState state_ = CircuitState::Closed;
    int32_t consecutive_failures_ = 0;
    std::chrono::steady_clock::time_point last_state_change_;
    std::mutex state_mutex_;

    void load_embedding_file();
    void record_success();
    void record_failure();
    bool check_circuit(); // Returns true if allowed to proceed

    // Low-level helper to perform the HTTP POST via libcurl
    std::string perform_post(const std::string& url, const std::string& payload);
};

} // namespace embedding
