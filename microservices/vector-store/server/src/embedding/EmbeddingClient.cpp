//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#include "embedding/EmbeddingClient.h"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <iostream>
#include <fstream>
#include <cstdlib>

namespace embedding {

using json = nlohmann::json;

// Libcurl callback helper to write received data into std::string buffer
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    std::string* mem = static_cast<std::string*>(userp);
    mem->append(static_cast<char*>(contents), realsize);
    return realsize;
}

EmbeddingClient::EmbeddingClient(
    const std::string& base_url,
    const std::string& model,
    int32_t timeout_ms,
    int32_t failure_threshold,
    int32_t cooldown_s
) : base_url_(base_url),
    model_(model),
    timeout_ms_(timeout_ms),
    failure_threshold_(failure_threshold),
    cooldown_s_(cooldown_s),
    state_(CircuitState::Closed),
    consecutive_failures_(0),
    last_state_change_(std::chrono::steady_clock::now()) {
    // Global curl initialization (safe if called multiple times or inside a single-threaded startup context)
    curl_global_init(CURL_GLOBAL_ALL);

    // Check if file-based embedding provider should be loaded
    const char* file_path_env = std::getenv("EMBEDDING_FILE_PATH");
    if (file_path_env && std::string(file_path_env).length() > 0) {
        use_file_ = true;
        file_path_ = std::string(file_path_env);
        std::cout << "[EmbeddingClient] Configured to use file as source of truth for embeddings: " << file_path_ << std::endl;
        load_embedding_file();
    }
}

EmbeddingClient::~EmbeddingClient() {
    curl_global_cleanup();
}

bool EmbeddingClient::isCircuitOpen() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    check_circuit();
    return state_ == CircuitState::Open;
}

int32_t EmbeddingClient::consecutiveFailures() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(state_mutex_));
    return consecutive_failures_;
}

bool EmbeddingClient::check_circuit() {
    auto now = std::chrono::steady_clock::now();
    if (state_ == CircuitState::Open) {
        if (now - last_state_change_ >= cooldown_s_) {
            state_ = CircuitState::HalfOpen;
            last_state_change_ = now;
            std::cout << "[CircuitBreaker] Cooldown elapsed. Circuit entering Half-Open state. Allowing probe request." << std::endl;
            return true;
        }
        return false;
    }
    return true;
}

void EmbeddingClient::record_success() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    consecutive_failures_ = 0;
    if (state_ != CircuitState::Closed) {
        state_ = CircuitState::Closed;
        last_state_change_ = std::chrono::steady_clock::now();
        std::cout << "[CircuitBreaker] Probe succeeded. Circuit CLOSED." << std::endl;
    }
}

void EmbeddingClient::record_failure() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    consecutive_failures_++;
    std::cerr << "[CircuitBreaker] Embedding request failed. Consecutive failures: " << consecutive_failures_ << std::endl;

    if (state_ != CircuitState::Open && consecutive_failures_ >= failure_threshold_) {
        state_ = CircuitState::Open;
        last_state_change_ = std::chrono::steady_clock::now();
        std::cerr << "[CircuitBreaker] Failure threshold reached. Circuit opened. Fast-failing future embedding requests." << std::endl;
    }
}

std::vector<float> EmbeddingClient::embed(const std::string& text) {
    auto results = embedBatch({text});
    if (results.empty()) {
        throw std::runtime_error("Empty embedding results returned from embedding service.");
    }
    return results[0];
}

void EmbeddingClient::load_embedding_file() {
    std::cout << "[EmbeddingClient] Loading, parsing, and validating embedding source-of-truth file: " << file_path_ << std::endl;
    std::ifstream file(file_path_);
    if (!file.is_open()) {
        std::cerr << "[EmbeddingClient] Error: Embedding file not found or could not be opened: " << file_path_ << std::endl;
        throw std::runtime_error("Embedding file not found or could not be opened: " + file_path_);
    }

    json root;
    try {
        file >> root;
    } catch (const std::exception& e) {
        std::cerr << "[EmbeddingClient] Error parsing JSON: " << e.what() << std::endl;
        throw std::runtime_error("Malformed JSON in embedding file: " + std::string(e.what()));
    }

    // Check basic format structure
    if (!root.contains("data")) {
        throw std::runtime_error("Malformed embedding file: Missing 'data' array in JSON root");
    }
    if (!root["data"].is_array()) {
        throw std::runtime_error("Malformed embedding file: 'data' is not a JSON array");
    }
    if (root["data"].empty()) {
        throw std::runtime_error("Malformed embedding file: 'data' array is empty");
    }

    // Load and validate all embeddings in the list
    size_t idx = 0;
    for (const auto& item : root["data"]) {
        if (!item.is_object()) {
            throw std::runtime_error("Malformed embedding file: data item is not a JSON object at index " + std::to_string(idx));
        }
        if (!item.contains("embedding")) {
            throw std::runtime_error("Malformed embedding file: missing 'embedding' field in data object at index " + std::to_string(idx));
        }

        const auto& emb_val = item["embedding"];
        std::vector<float> vec;

        // Support both direct array format [0.1, 0.2, ...] and nested actual_instance format
        if (emb_val.is_array()) {
            vec = emb_val.get<std::vector<float>>();
        } else if (emb_val.is_object() && emb_val.contains("actual_instance")) {
            const auto& actual = emb_val["actual_instance"];
            if (!actual.is_array()) {
                throw std::runtime_error("Malformed embedding file: 'actual_instance' is not an array at index " + std::to_string(idx));
            }
            vec = actual.get<std::vector<float>>();
        } else {
            throw std::runtime_error("Malformed embedding file: 'embedding' is neither an array nor an object with 'actual_instance' at index " + std::to_string(idx));
        }

        if (vec.empty()) {
            throw std::runtime_error("Malformed embedding file: empty embedding vector at index " + std::to_string(idx));
        }

        // Dynamically resize/pad the vector to 768 dimensions (the static database pgvector constraint in 001_initial_schema.sql)
        // to prevent pgvector dimension mismatch errors during document insertion and similarity searches.
        if (vec.size() != 768) {
            std::cout << "[EmbeddingClient] Warning: Loaded vector has " << vec.size()
                      << " dimensions. Resizing/padding to exactly 768 dimensions for database compatibility." << std::endl;
            vec.resize(768, 0.0f);
        }

        file_embeddings_.push_back(vec);
        idx++;
    }

    std::cout << "[EmbeddingClient] Successfully loaded and validated " << file_embeddings_.size()
              << " embedding vector(s) from " << file_path_ << ". Dimensions: "
              << file_embeddings_[0].size() << std::endl;
}

std::vector<std::vector<float>> EmbeddingClient::embedBatch(const std::vector<std::string>& texts) {
    if (texts.empty()) {
        return {};
    }

    // 1. If configured to use static file source of truth, bypass network entirely
    if (use_file_) {
        if (file_embeddings_.empty()) {
            throw std::runtime_error("Embedding file loaded, but no valid embedding vectors are cached.");
        }
        std::vector<std::vector<float>> results;
        results.reserve(texts.size());
        for (size_t i = 0; i < texts.size(); ++i) {
            // Cycle through available embeddings in the file
            results.push_back(file_embeddings_[i % file_embeddings_.size()]);
        }
        return results;
    }

    // 2. Otherwise fall back to network-based service (HTTP + Circuit Breaker)
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!check_circuit()) {
            throw std::runtime_error("Embedding service circuit breaker is OPEN. Request blocked.");
        }
    }

    std::vector<std::vector<float>> embeddings(texts.size());
    for (size_t i = 0; i < texts.size(); ++i) {
        json payload = json::object();
        payload["input"] = texts[i];

        std::string response_data;
        try {
            std::string url = base_url_ + "/v1/embeddings";
            response_data = perform_post(url, payload.dump());
        } catch (const std::exception& e) {
            record_failure();
            throw;
        }

        try {
            json res = json::parse(response_data);
            if (res.contains("error")) {
                throw std::runtime_error(res["error"]["message"].get<std::string>());
            }

            if (res.contains("data") && res["data"].is_array() && !res["data"].empty()) {
                const auto& item = res["data"][0];
                if (item.contains("embedding")) {
                    const auto& emb_val = item.at("embedding");
                    if (emb_val.is_array()) {
                        embeddings[i] = emb_val.get<std::vector<float>>();
                    } else if (emb_val.is_object() && emb_val.contains("actual_instance")) {
                        embeddings[i] = emb_val.at("actual_instance").get<std::vector<float>>();
                    }
                }
            } else if (res.contains("data") && res["data"].is_object()) {
                const auto& item = res["data"];
                if (item.contains("embedding")) {
                    const auto& emb_val = item.at("embedding");
                    if (emb_val.is_array()) {
                        embeddings[i] = emb_val.get<std::vector<float>>();
                    } else if (emb_val.is_object() && emb_val.contains("actual_instance")) {
                        embeddings[i] = emb_val.at("actual_instance").get<std::vector<float>>();
                    }
                }
            }

            // Ensure output embeddings have exactly 768 dimensions for database compatibility
            if (embeddings[i].size() != 768) {
                embeddings[i].resize(768, 0.0f);
            }
        } catch (const std::exception& e) {
            record_failure();
            std::cerr << "[EmbeddingClient] Failed to parse API response: " << response_data << std::endl;
            throw std::runtime_error(std::string("Embedding API response parsing failed: ") + e.what());
        }
    }

    record_success();
    return embeddings;
}

std::string EmbeddingClient::perform_post(const std::string& url, const std::string& payload) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize libcurl Easy handle.");
    }

    std::string response_buffer;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, payload.length());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buffer);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms_);

    CURLcode rc = curl_easy_perform(curl);

    // Clean up headers and handle
    curl_slist_free_all(headers);

    long http_code = 0;
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    }
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        throw std::runtime_error(std::string("curl_easy_perform failed: ") + curl_easy_strerror(rc));
    }

    if (http_code >= 400) {
        throw std::runtime_error("HTTP post request failed with code: " + std::to_string(http_code));
    }

    return response_buffer;
}

} // namespace embedding
