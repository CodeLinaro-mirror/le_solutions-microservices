//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#pragma once

#include "db/PgConnectionPool.h"
#include "db/VectorStoreRepository.h"
#include "db/SchemaMigrator.h"
#include "embedding/EmbeddingClient.h"
#include "jobs/IngestionJobQueue.h"
#include "VectorStoreDTOs.h"
#include <memory>
#include <string>

namespace store {

/**
 * @brief Singleton Orchestrator that coordinates database pools, clients, and async threads.
 * Bootstrapped on service startup via environment variables.
 */
class VectorStoreManager {
public:
    static VectorStoreManager& getInstance();

    // Prevent copies/moves
    VectorStoreManager(const VectorStoreManager&) = delete;
    VectorStoreManager& operator=(const VectorStoreManager&) = delete;
    VectorStoreManager(VectorStoreManager&&) = delete;
    VectorStoreManager& operator=(VectorStoreManager&&) = delete;

    // Lifecyle operations
    void initialize();
    void shutdown();

    // Accessors for controllers
    db::VectorStoreRepository& repository();
    embedding::EmbeddingClient& embedding_client();
    jobs::IngestionJobQueue& job_queue();

    // High level orchestration methods
    std::string enqueueIngestion(
        const std::string& store_id,
        const std::string& file_id,
        std::vector<DocumentChunk> chunks
    );

    SearchResponse searchStore(
        const std::string& store_id,
        const SearchRequest& request
    );

private:
    VectorStoreManager();
    ~VectorStoreManager();

    std::unique_ptr<db::PgConnectionPool> pool_;
    std::unique_ptr<db::VectorStoreRepository> repo_;
    std::unique_ptr<db::SchemaMigrator> migrator_;
    std::unique_ptr<embedding::EmbeddingClient> embed_client_;
    std::unique_ptr<jobs::IngestionJobQueue> job_queue_;

    bool is_initialized_ = false;
};

} // namespace store
