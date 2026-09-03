//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#include "store/VectorStoreManager.h"
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <chrono>
#include <algorithm>
#include <cctype>

namespace store {

// Static helper to load environment variables from a .env file
static void loadDotEnv() {
    std::ifstream env_file(".env");
    if (!env_file.is_open()) {
        env_file.open("/app/.env");
    }
    if (env_file.is_open()) {
        std::cout << "[VectorStoreManager] Loading variables from .env file..." << std::endl;
        std::string line;
        while (std::getline(env_file, line)) {
            // Trim whitespace
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) line.erase(0, 1);
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();

            if (line.empty() || line[0] == '#') continue;

            auto eq_pos = line.find('=');
            if (eq_pos == std::string::npos) continue;

            std::string key = line.substr(0, eq_pos);
            std::string val = line.substr(eq_pos + 1);

            // Trim key
            while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
            // Trim val quotes and spaces
            while (!val.empty() && std::isspace(static_cast<unsigned char>(val.front()))) val.erase(0, 1);
            while (!val.empty() && std::isspace(static_cast<unsigned char>(val.back()))) val.pop_back();

            if (val.length() >= 2 && val.front() == '"' && val.back() == '"') {
                val = val.substr(1, val.length() - 2);
            } else if (val.length() >= 2 && val.front() == '\'' && val.back() == '\'') {
                val = val.substr(1, val.length() - 2);
            }

            // Set environment variable (overwrite if already set)
            setenv(key.c_str(), val.c_str(), 1);
        }
        env_file.close();
    } else {
        std::cout << "[VectorStoreManager] No .env file found. Using system environment variables." << std::endl;
    }
}

// Helper to safely read string env variables with defaults
static std::string get_env_string(const char* name, const std::string& default_val) {
    const char* val = std::getenv(name);
    return val ? std::string(val) : default_val;
}

// Helper to safely read int32_t env variables with defaults
static int32_t get_env_int(const char* name, int32_t default_val) {
    const char* val = std::getenv(name);
    try {
        return val ? std::stoi(val) : default_val;
    } catch (...) {
        return default_val;
    }
}

VectorStoreManager& VectorStoreManager::getInstance() {
    static VectorStoreManager instance;
    return instance;
}

VectorStoreManager::VectorStoreManager() = default;
VectorStoreManager::~VectorStoreManager() {
    shutdown();
}

void VectorStoreManager::initialize() {
    if (is_initialized_) return;

    std::cout << "[VectorStoreManager] Initializing platform components..." << std::endl;

    // Load .env configuration
    loadDotEnv();

    // 1. Resolve configuration from environment variables with robust validation
    std::string pg_conn = get_env_string("VECTOR_STORE_PG_CONN", "");
    if (pg_conn.empty()) {
        std::cerr << "❌ Fatal Error: VECTOR_STORE_PG_CONN environment variable is required but empty!" << std::endl;
        throw std::runtime_error("Configuration error: VECTOR_STORE_PG_CONN is required but missing.");
    }

    int32_t pool_size = get_env_int("VECTOR_STORE_PG_POOL_SIZE", 10);

    std::string embedding_url = get_env_string("EMBEDDING_SERVICE_URL", "");
    if (embedding_url.empty()) {
        std::cerr << "❌ Fatal Error: EMBEDDING_SERVICE_URL environment variable is required but empty!" << std::endl;
        throw std::runtime_error("Configuration error: EMBEDDING_SERVICE_URL is required but missing.");
    }

    int32_t embed_timeout = get_env_int("EMBEDDING_SERVICE_TIMEOUT_MS", 10000);
    int32_t embed_failure_threshold = get_env_int("EMBEDDING_CIRCUIT_BREAKER_THRESHOLD", 5);
    int32_t embed_cooldown = get_env_int("EMBEDDING_CIRCUIT_BREAKER_COOLDOWN_S", 30);

    int32_t ingest_batch_size = get_env_int("VECTOR_STORE_INGEST_BATCH_SIZE", 32);
    int32_t ingest_workers = get_env_int("VECTOR_STORE_INGEST_WORKERS", 2);
    std::string migrations_dir = get_env_string("VECTOR_STORE_MIGRATIONS_DIR", "./migrations");

    // 2. Instantiate PostgreSQL Connection Pool
    try {
        pool_ = std::make_unique<db::PgConnectionPool>(pg_conn, pool_size);
        std::cout << "[VectorStoreManager] Connected to database with pool size: " << pool_size << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[VectorStoreManager] Fatal: Could not establish database pool! " << e.what() << std::endl;
        throw;
    }

    // 3. Instantiate Database Repository
    repo_ = std::make_unique<db::VectorStoreRepository>(*pool_);

    // 4. Run database migrations at service boot up (fails fast if error or pgvector missing)
    try {
        migrator_ = std::make_unique<db::SchemaMigrator>(*pool_, migrations_dir);
        std::cout << "[VectorStoreManager] Running database migrations from " << migrations_dir << "..." << std::endl;
        int32_t current_ver = migrator_->migrate();
        std::cout << "[VectorStoreManager] Migrations successful. Current database schema version: " << current_ver << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[VectorStoreManager] Fatal: Schema migration failed! " << e.what() << std::endl;
        throw;
    }

    // 5. Instantiate Embedding Service Client
    std::string embed_model = get_env_string("EMBEDDING_MODEL_NAME", "nomic-embed-text");
    embed_client_ = std::make_unique<embedding::EmbeddingClient>(
        embedding_url,
        embed_model,
        embed_timeout,
        embed_failure_threshold,
        embed_cooldown
    );

    // 6. Instantiate Background Ingestion Queue
    job_queue_ = std::make_unique<jobs::IngestionJobQueue>(*repo_, *embed_client_, ingest_workers, ingest_batch_size);
    job_queue_->start();

    is_initialized_ = true;
    std::cout << "[VectorStoreManager] Platform boot up sequence completed successfully." << std::endl;
}

void VectorStoreManager::shutdown() {
    if (!is_initialized_) return;

    std::cout << "[VectorStoreManager] Shutting down platform components..." << std::endl;

    // Stop ingestion workers
    if (job_queue_) {
        job_queue_->stop();
    }

    is_initialized_ = false;
    std::cout << "[VectorStoreManager] Shutdown completed successfully." << std::endl;
}

db::VectorStoreRepository& VectorStoreManager::repository() {
    if (!is_initialized_) throw std::runtime_error("VectorStoreManager is not initialized.");
    return *repo_;
}

embedding::EmbeddingClient& VectorStoreManager::embedding_client() {
    if (!is_initialized_) throw std::runtime_error("VectorStoreManager is not initialized.");
    return *embed_client_;
}

jobs::IngestionJobQueue& VectorStoreManager::job_queue() {
    if (!is_initialized_) throw std::runtime_error("VectorStoreManager is not initialized.");
    return *job_queue_;
}

std::string VectorStoreManager::enqueueIngestion(
    const std::string& store_id,
    const std::string& file_id,
    std::vector<DocumentChunk> chunks
) {
    if (!is_initialized_) throw std::runtime_error("VectorStoreManager is not initialized.");

    // Validate that store exists (will throw if not exists)
    repo_->getStore(store_id);

    int32_t count = chunks.size();

    // Create tracking job in database (P0 Async Requirement)
    std::string job_id = repo_->createJob(store_id, count);

    // Dispatch processing to background workers thread pool
    job_queue_->enqueueJob(job_id, store_id, file_id, std::move(chunks));

    return job_id;
}

SearchResponse VectorStoreManager::searchStore(
    const std::string& store_id,
    const SearchRequest& request
) {
    if (!is_initialized_) throw std::runtime_error("VectorStoreManager is not initialized.");

    // 1. Check Circuit Breaker (P0 Circuit Breaker Requirement)
    if (embed_client_->isCircuitOpen()) {
        throw std::runtime_error("Circuit Breaker is OPEN. Embedding service is currently unreachable.");
    }

    // Get store details to extract default configs
    auto store = repo_->getStore(store_id);

    // 2. Fetch Embeddings for Query
    auto start_embed = std::chrono::high_resolution_clock::now();
    std::vector<float> query_vector;
    try {
        query_vector = embed_client_->embed(request.query);
    } catch (const std::exception& e) {
        std::cerr << "[VectorStoreManager] Failed to get query embedding: " << e.what() << std::endl;
        throw;
    }
    auto end_embed = std::chrono::high_resolution_clock::now();
    int32_t embed_latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_embed - start_embed).count();

    // 3. Search Pgvector Database (Filtered HNSW query)
    auto start_search = std::chrono::high_resolution_clock::now();
    auto results = repo_->search(
        store_id,
        query_vector,
        request.top_k,
        request.score_threshold,
        request.filter,
        store.hnsw_ef_search
    );
    auto end_search = std::chrono::high_resolution_clock::now();
    int32_t search_latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_search - start_search).count();

    // 4. Formulate Search Response Contract (P0 Empty Result Contract Requirement)
    SearchResponse response;
    response.results = results;
    response.embedding_latency_ms = embed_latency_ms;
    response.search_latency_ms = search_latency_ms;

    if (results.empty()) {
        response.retrieval_status = "no_match";
        response.best_score_found = 0.0f;
    } else {
        response.retrieval_status = "ok";
        response.best_score_found = results[0].score;
    }

    return response;
}

} // namespace store
