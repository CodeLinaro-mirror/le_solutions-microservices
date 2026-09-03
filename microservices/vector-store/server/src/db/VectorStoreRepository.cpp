//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#include "db/VectorStoreRepository.h"
#include <chrono>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <iomanip>
#include <iostream>
#include <uuid/uuid.h> // For job ID generation

namespace db {

// Helper to generate a random UUID string
static std::string generate_uuid() {
    uuid_t buuid;
    uuid_generate_random(buuid);
    char uuid_str[37];
    uuid_unparse_lower(buuid, uuid_str);
    return std::string(uuid_str);
}

// Helper to get current epoch time in milliseconds
static long long current_epoch_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

VectorStoreRepository::VectorStoreRepository(PgConnectionPool& pool)
    : pool_(pool) {}

std::string VectorStoreRepository::to_vector_string(const std::vector<float>& vec) const {
    std::ostringstream ss;
    ss << "[";
    for (size_t i = 0; i < vec.size(); ++i) {
        ss << std::fixed << std::setprecision(6) << vec[i];
        if (i < vec.size() - 1) {
            ss << ",";
        }
    }
    ss << "]";
    return ss.str();
}

store::VectorStore VectorStoreRepository::createStore(const store::CreateStoreRequest& req) {
    auto guard = pool_.acquire();
    pqxx::work tx(guard.conn());

    long long now = current_epoch_ms();

    tx.exec_params(
        "INSERT INTO vector_stores (id, name, embedding_model, embedding_dim, hnsw_m, hnsw_ef_construction, hnsw_ef_search, status, metadata, created_at) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)",
        req.id,
        req.name,
        req.embedding_model,
        req.embedding_dim,
        req.hnsw_m,
        req.hnsw_ef_construction,
        req.hnsw_ef_search,
        "active",
        req.metadata.dump(),
        now
    );

    tx.commit();

    return store::VectorStore{
        req.id,
        req.name,
        req.embedding_model,
        req.embedding_dim,
        req.hnsw_m,
        req.hnsw_ef_construction,
        req.hnsw_ef_search,
        "active",
        req.metadata,
        now
    };
}

store::VectorStore VectorStoreRepository::getStore(const std::string& id) {
    auto guard = pool_.acquire();
    pqxx::nontransaction tx(guard.conn());

    pqxx::result res = tx.exec_params(
        "SELECT id, name, embedding_model, embedding_dim, hnsw_m, hnsw_ef_construction, hnsw_ef_search, status, metadata, created_at "
        "FROM vector_stores WHERE id = $1",
        id
    );

    if (res.empty()) {
        throw std::runtime_error("Vector store not found: " + id);
    }

    auto row = res[0];
    return store::VectorStore{
        row[0].as<std::string>(),
        row[1].as<std::string>(),
        row[2].as<std::string>(),
        row[3].as<int32_t>(),
        row[4].as<int32_t>(),
        row[5].as<int32_t>(),
        row[6].as<int32_t>(),
        row[7].as<std::string>(),
        nlohmann::json::parse(row[8].as<std::string>()),
        row[9].as<long long>()
    };
}

std::vector<store::VectorStore> VectorStoreRepository::listStores() {
    auto guard = pool_.acquire();
    pqxx::nontransaction tx(guard.conn());

    pqxx::result res = tx.exec("SELECT id, name, embedding_model, embedding_dim, hnsw_m, hnsw_ef_construction, hnsw_ef_search, status, metadata, created_at FROM vector_stores");

    std::vector<store::VectorStore> stores;
    for (const auto& row : res) {
        stores.push_back(store::VectorStore{
            row[0].as<std::string>(),
            row[1].as<std::string>(),
            row[2].as<std::string>(),
            row[3].as<int32_t>(),
            row[4].as<int32_t>(),
            row[5].as<int32_t>(),
            row[6].as<int32_t>(),
            row[7].as<std::string>(),
            nlohmann::json::parse(row[8].as<std::string>()),
            row[9].as<long long>()
        });
    }
    return stores;
}

void VectorStoreRepository::deleteStore(const std::string& id) {
    auto guard = pool_.acquire();
    pqxx::work tx(guard.conn());

    tx.exec_params("DELETE FROM vector_stores WHERE id = $1", id);
    tx.commit();
}

std::vector<std::string> VectorStoreRepository::getDatabaseStats() {
    try {
        auto guard = pool_.acquire();
        pqxx::nontransaction tx(guard.conn());
        pqxx::result res = tx.exec(
            "SELECT pg_size_pretty(pg_database_size('vectordb')), "
            "       pg_size_pretty(pg_total_relation_size('vector_store_documents')), "
            "       pg_size_pretty(pg_relation_size('idx_vsd_embedding_hnsw'))"
        );
        if (!res.empty()) {
            return {
                res[0][0].as<std::string>(),
                res[0][1].as<std::string>(),
                res[0][2].as<std::string>()
            };
        }
    } catch (...) {}
    return {"0 kB", "0 kB", "0 kB"};
}

store::VectorStore VectorStoreRepository::updateStore(const std::string& id, const std::string& name, const nlohmann::json& metadata) {
    auto guard = pool_.acquire();
    pqxx::work tx(guard.conn());

    // 1. Fetch current store to confirm it exists and to preserve fields not being updated
    pqxx::result res = tx.exec_params(
        "SELECT id, name, embedding_model, embedding_dim, hnsw_m, hnsw_ef_construction, hnsw_ef_search, status, metadata, created_at "
        "FROM vector_stores WHERE id = $1",
        id
    );

    if (res.empty()) {
        throw std::runtime_error("Vector store not found for update: " + id);
    }

    auto row = res[0];
    std::string current_name = row[1].as<std::string>();
    std::string current_metadata_str = row[8].as<std::string>();

    std::string updated_name = name.empty() ? current_name : name;
    nlohmann::json updated_metadata = metadata.is_null() || metadata.empty() ? nlohmann::json::parse(current_metadata_str) : metadata;

    // 2. Perform the update query
    tx.exec_params(
        "UPDATE vector_stores SET name = $1, metadata = $2 WHERE id = $3",
        updated_name,
        updated_metadata.dump(),
        id
    );

    tx.commit();

    return store::VectorStore{
        row[0].as<std::string>(),
        updated_name,
        row[2].as<std::string>(),
        row[3].as<int32_t>(),
        row[4].as<int32_t>(),
        row[5].as<int32_t>(),
        row[6].as<int32_t>(),
        row[7].as<std::string>(),
        updated_metadata,
        row[9].as<long long>()
    };
}

void VectorStoreRepository::insertDocuments(
    const std::string& store_id,
    const std::string& file_id,
    const std::vector<store::DocumentChunk>& chunks,
    const std::vector<std::vector<float>>& embeddings
) {
    if (chunks.size() != embeddings.size()) {
        throw std::invalid_argument("Chunks and embeddings size mismatch in insertDocuments");
    }

    auto guard = pool_.acquire();
    pqxx::work tx(guard.conn());

    long long now = current_epoch_ms();

    for (size_t i = 0; i < chunks.size(); ++i) {
        std::string vec_str = to_vector_string(embeddings[i]);
        tx.exec_params(
            "INSERT INTO vector_store_documents (vector_store_id, file_id, text_chunk, metadata, embedding, created_at) "
            "VALUES ($1, $2, $3, $4, $5, $6)",
            store_id,
            file_id.empty() ? std::optional<std::string>{} : std::optional<std::string>{file_id},
            chunks[i].text,
            chunks[i].metadata.dump(),
            vec_str,
            now
        );
    }

    tx.commit();
}

std::vector<store::SearchResult> VectorStoreRepository::search(
    const std::string& store_id,
    const std::vector<float>& query_vector,
    int32_t top_k,
    float score_threshold,
    const nlohmann::json& metadata_filter,
    int32_t ef_search
) {
    auto guard = pool_.acquire();
    pqxx::work tx(guard.conn());

    // Apply the query-time session beam width parameter for HNSW
    // Note: SET does not support parameterized queries in PostgreSQL; ef_search is an integer so this is safe.
    tx.exec("SET hnsw.ef_search = " + std::to_string(ef_search));

    std::string vec_str = to_vector_string(query_vector);
    std::string filter_str = metadata_filter.is_null() ? "{}" : metadata_filter.dump();

    // Use <#> Inner Product Distance (for L2 normalized vectors, Cosine Similarity is simply the Dot Product, which is -(embedding <#> $1))
    std::string sql_query =
        "SELECT id, text_chunk, metadata, file_id, "
        "       -(embedding <#> $1) AS score "
        "FROM vector_store_documents "
        "WHERE vector_store_id = $2 "
        "  AND deleted_at IS NULL "
        "  AND ($3 = '{}'::jsonb OR metadata @> $3) "
        "  AND -(embedding <#> $1) >= $4 "
        "ORDER BY embedding <#> $1 "
        "LIMIT $5";

    pqxx::result res = tx.exec_params(
        sql_query,
        vec_str,
        store_id,
        filter_str,
        score_threshold,
        top_k
    );

    std::vector<store::SearchResult> results;
    for (const auto& row : res) {
        results.push_back(store::SearchResult{
            std::to_string(row[0].as<long long>()),
            row[4].as<float>(),
            row[1].as<std::string>(),
            nlohmann::json::parse(row[2].as<std::string>()),
            row[3].as<std::optional<std::string>>().value_or("")
        });
    }

    tx.commit();
    return results;
}

store::DocumentWithEmbedding VectorStoreRepository::getDocumentWithEmbedding(
    const std::string& store_id,
    const std::string& doc_id
) {
    auto guard = pool_.acquire();
    pqxx::nontransaction tx(guard.conn());

    pqxx::result res = tx.exec_params(
        "SELECT id, vector_store_id, file_id, text_chunk, metadata, embedding::text, created_at "
        "FROM vector_store_documents "
        "WHERE vector_store_id = $1 AND id = $2 AND deleted_at IS NULL",
        store_id,
        doc_id
    );

    if (res.empty()) {
        throw std::runtime_error("Document not found: id=" + doc_id + " in store=" + store_id);
    }

    auto row = res[0];

    // Parse the pgvector string "[0.1,0.2,...]" into std::vector<float>
    std::vector<float> embedding;
    if (!row[5].is_null()) {
        std::string vec_str = row[5].as<std::string>();
        // Strip surrounding brackets
        if (!vec_str.empty() && vec_str.front() == '[') vec_str = vec_str.substr(1);
        if (!vec_str.empty() && vec_str.back() == ']') vec_str.pop_back();
        std::istringstream ss(vec_str);
        std::string token;
        while (std::getline(ss, token, ',')) {
            try { embedding.push_back(std::stof(token)); } catch (...) {}
        }
    }

    return store::DocumentWithEmbedding{
        std::to_string(row[0].as<long long>()),
        row[1].as<std::string>(),
        row[2].as<std::optional<std::string>>().value_or(""),
        row[3].as<std::string>(),
        nlohmann::json::parse(row[4].as<std::string>()),
        embedding,
        row[6].as<long long>()
    };
}

void VectorStoreRepository::deleteFile(const std::string& store_id, const std::string& file_id) {
    auto guard = pool_.acquire();
    pqxx::work tx(guard.conn());

    long long now = current_epoch_ms();
    // Soft delete files matching store and file_id
    tx.exec_params(
        "UPDATE vector_store_documents SET deleted_at = $1 "
        "WHERE vector_store_id = $2 AND file_id = $3 AND deleted_at IS NULL",
        now,
        store_id,
        file_id
    );

    tx.commit();
}

// ================= Job Tracking =================

std::string VectorStoreRepository::createJob(const std::string& store_id, int32_t total_chunks) {
    auto guard = pool_.acquire();
    pqxx::work tx(guard.conn());

    std::string job_id = "job_" + generate_uuid().substr(0, 8);
    long long now = current_epoch_ms();

    tx.exec_params(
        "INSERT INTO ingestion_jobs (id, vector_store_id, status, total_chunks, processed_chunks, failed_chunks, created_at) "
        "VALUES ($1, $2, $3, $4, $5, $6, $7)",
        job_id,
        store_id,
        "processing",
        total_chunks,
        0,
        0,
        now
    );

    tx.commit();
    return job_id;
}

void VectorStoreRepository::updateJobProgress(const std::string& job_id, int32_t processed, int32_t failed) {
    auto guard = pool_.acquire();
    pqxx::work tx(guard.conn());

    tx.exec_params(
        "UPDATE ingestion_jobs "
        "SET processed_chunks = processed_chunks + $1, failed_chunks = failed_chunks + $2 "
        "WHERE id = $3",
        processed,
        failed,
        job_id
    );

    tx.commit();
}

void VectorStoreRepository::completeJob(const std::string& job_id) {
    auto guard = pool_.acquire();
    pqxx::work tx(guard.conn());

    long long now = current_epoch_ms();

    tx.exec_params(
        "UPDATE ingestion_jobs "
        "SET status = 'completed', completed_at = $1 "
        "WHERE id = $2",
        now,
        job_id
    );

    tx.commit();
}

void VectorStoreRepository::failJob(const std::string& job_id, const std::string& error_msg) {
    auto guard = pool_.acquire();
    pqxx::work tx(guard.conn());

    long long now = current_epoch_ms();

    tx.exec_params(
        "UPDATE ingestion_jobs "
        "SET status = 'failed', error_message = $1, completed_at = $2 "
        "WHERE id = $3",
        error_msg,
        now,
        job_id
    );

    tx.commit();
}

store::IngestionJob VectorStoreRepository::getJob(const std::string& job_id) {
    auto guard = pool_.acquire();
    pqxx::nontransaction tx(guard.conn());

    pqxx::result res = tx.exec_params(
        "SELECT id, vector_store_id, status, total_chunks, processed_chunks, failed_chunks, error_message, created_at, completed_at "
        "FROM ingestion_jobs WHERE id = $1",
        job_id
    );

    if (res.empty()) {
        throw std::runtime_error("Ingestion job not found: " + job_id);
    }

    auto row = res[0];
    return store::IngestionJob{
        row[0].as<std::string>(),
        row[1].as<std::string>(),
        row[2].as<std::string>(),
        row[3].as<int32_t>(),
        row[4].as<int32_t>(),
        row[5].as<int32_t>(),
        row[6].as<std::optional<std::string>>().value_or(""),
        row[7].as<long long>(),
        row[8].as<std::optional<long long>>().value_or(0LL)
    };
}

std::vector<store::IngestionJob> VectorStoreRepository::listJobs(const std::string& store_id) {
    auto guard = pool_.acquire();
    pqxx::nontransaction tx(guard.conn());

    pqxx::result res = tx.exec_params(
        "SELECT id, vector_store_id, status, total_chunks, processed_chunks, failed_chunks, error_message, created_at, completed_at "
        "FROM ingestion_jobs WHERE vector_store_id = $1 ORDER BY created_at DESC",
        store_id
    );

    std::vector<store::IngestionJob> jobs;
    for (const auto& row : res) {
        try {
            std::string id = row[0].as<std::string>();
            std::string store_id_val = row[1].as<std::string>();
            std::string status = row[2].as<std::string>();
            int32_t total = row[3].as<int32_t>();
            int32_t proc = row[4].as<int32_t>();
            int32_t fail = row[5].as<int32_t>();

            std::string err_msg = "";
            if (!row[6].is_null()) {
                err_msg = row[6].as<std::string>();
            }

            long long created = row[7].as<long long>();

            long long completed = 0LL;
            if (!row[8].is_null()) {
                completed = row[8].as<long long>();
            }

            jobs.push_back(store::IngestionJob{
                id, store_id_val, status, total, proc, fail, err_msg, created, completed
            });
        } catch (const std::exception& e) {
            std::cerr << "[VectorStoreRepository] listJobs field parsing crashed: " << e.what() << std::endl;
            for (size_t col = 0; col < row.size(); ++col) {
                std::cerr << "  col " << col << " null: " << (row[col].is_null() ? "yes" : "no")
                          << " name: " << row[col].name() << std::endl;
            }
            throw;
        }
    }
    return jobs;
}

} // namespace db
