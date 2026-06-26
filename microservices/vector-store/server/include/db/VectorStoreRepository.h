//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#pragma once

#include "db/PgConnectionPool.h"
#include "VectorStoreDTOs.h"
#include <string>
#include <vector>

namespace db {

class VectorStoreRepository {
public:
    VectorStoreRepository(PgConnectionPool& pool);

    // ================= Store CRUD Operations =================
    store::VectorStore createStore(const store::CreateStoreRequest& req);
    store::VectorStore getStore(const std::string& id);
    std::vector<store::VectorStore> listStores();
    store::VectorStore updateStore(const std::string& id, const std::string& name, const nlohmann::json& metadata);
    void deleteStore(const std::string& id);
    std::vector<std::string> getDatabaseStats();

    // ================= Document Operations =================
    void insertDocuments(
        const std::string& store_id,
        const std::string& file_id,
        const std::vector<store::DocumentChunk>& chunks,
        const std::vector<std::vector<float>>& embeddings
    );

    std::vector<store::SearchResult> search(
        const std::string& store_id,
        const std::vector<float>& query_vector,
        int32_t top_k,
        float score_threshold,
        const nlohmann::json& metadata_filter,
        int32_t ef_search
    );

    store::DocumentWithEmbedding getDocumentWithEmbedding(
        const std::string& store_id,
        const std::string& doc_id
    );

    void deleteFile(const std::string& store_id, const std::string& file_id);

    // ================= Job Tracking Operations =================
    std::string createJob(const std::string& store_id, int32_t total_chunks);
    void updateJobProgress(const std::string& job_id, int32_t processed, int32_t failed);
    void completeJob(const std::string& job_id);
    void failJob(const std::string& job_id, const std::string& error_msg);
    store::IngestionJob getJob(const std::string& job_id);
    std::vector<store::IngestionJob> listJobs(const std::string& store_id);

private:
    PgConnectionPool& pool_;

    // Internal helper to convert double to string in pgvector format: [0.1,0.2,...]
    std::string to_vector_string(const std::vector<float>& vec) const;
};

} // namespace db
