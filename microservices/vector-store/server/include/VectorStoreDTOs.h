//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace store {

using json = nlohmann::json;

// ================= Store CRUD DTOs =================

struct CreateStoreRequest {
    std::string id;
    std::string name;
    std::string embedding_model = "nomic-embed-text-v1";
    int32_t embedding_dim = 768;
    int32_t hnsw_m = 16;
    int32_t hnsw_ef_construction = 64;
    int32_t hnsw_ef_search = 40;
    json metadata = json::object();
};

inline void from_json(const json& j, CreateStoreRequest& r) {
    j.at("id").get_to(r.id);
    j.at("name").get_to(r.name);
    if (j.contains("embedding_model")) j.at("embedding_model").get_to(r.embedding_model);
    if (j.contains("embedding_dim")) j.at("embedding_dim").get_to(r.embedding_dim);
    if (j.contains("hnsw_m")) j.at("hnsw_m").get_to(r.hnsw_m);
    if (j.contains("hnsw_ef_construction")) j.at("hnsw_ef_construction").get_to(r.hnsw_ef_construction);
    if (j.contains("hnsw_ef_search")) j.at("hnsw_ef_search").get_to(r.hnsw_ef_search);
    if (j.contains("metadata")) r.metadata = j.at("metadata");
}

struct VectorStore {
    std::string id;
    std::string name;
    std::string embedding_model;
    int32_t embedding_dim;
    int32_t hnsw_m;
    int32_t hnsw_ef_construction;
    int32_t hnsw_ef_search;
    std::string status;
    json metadata;
    long long created_at;
};

inline void to_json(json& j, const VectorStore& s) {
    j = json{
        {"id", s.id},
        {"name", s.name},
        {"embedding_model", s.embedding_model},
        {"embedding_dim", s.embedding_dim},
        {"hnsw_m", s.hnsw_m},
        {"hnsw_ef_construction", s.hnsw_ef_construction},
        {"hnsw_ef_search", s.hnsw_ef_search},
        {"status", s.status},
        {"metadata", s.metadata},
        {"created_at", s.created_at}
    };
}

// ================= File Ingestion DTOs =================

struct DocumentChunk {
    std::string text;
    json metadata = json::object();
};

inline void from_json(const json& j, DocumentChunk& c) {
    j.at("text").get_to(c.text);
    if (j.contains("metadata")) c.metadata = j.at("metadata");
}

inline void to_json(json& j, const DocumentChunk& c) {
    j = json{
        {"text", c.text},
        {"metadata", c.metadata}
    };
}

struct IngestDocumentsRequest {
    std::string file_id;
    std::vector<DocumentChunk> chunks;
};

inline void from_json(const json& j, IngestDocumentsRequest& r) {
    if (j.contains("file_id")) j.at("file_id").get_to(r.file_id);
    j.at("chunks").get_to(r.chunks);
}

struct IngestionJob {
    std::string id;
    std::string vector_store_id;
    std::string status;
    int32_t total_chunks;
    int32_t processed_chunks;
    int32_t failed_chunks;
    std::string error_message;
    long long created_at;
    long long completed_at = 0;
};

inline void to_json(json& j, const IngestionJob& job) {
    j = json{
        {"id", job.id},
        {"vector_store_id", job.vector_store_id},
        {"status", job.status},
        {"total_chunks", job.total_chunks},
        {"processed_chunks", job.processed_chunks},
        {"failed_chunks", job.failed_chunks},
        {"created_at", job.created_at}
    };
    if (job.error_message.empty()) {
        j["error_message"] = nullptr;
    } else {
        j["error_message"] = job.error_message;
    }
    if (job.completed_at == 0) {
        j["completed_at"] = nullptr;
    } else {
        j["completed_at"] = job.completed_at;
    }
}

// ================= Document with Embedding DTO =================

struct DocumentWithEmbedding {
    std::string id;
    std::string vector_store_id;
    std::string file_id;
    std::string text_chunk;
    json metadata;
    std::vector<float> embedding;
    long long created_at;
};

inline void to_json(json& j, const DocumentWithEmbedding& d) {
    j = json{
        {"id", d.id},
        {"vector_store_id", d.vector_store_id},
        {"file_id", d.file_id},
        {"text_chunk", d.text_chunk},
        {"metadata", d.metadata},
        {"embedding", d.embedding},
        {"embedding_dimensions", d.embedding.size()},
        {"created_at", d.created_at}
    };
}

// ================= Semantic Search DTOs =================

struct SearchRequest {
    std::string query;
    int32_t top_k = 5;
    float score_threshold = 0.65f;
    json filter = json::object();
};

inline void from_json(const json& j, SearchRequest& r) {
    j.at("query").get_to(r.query);
    if (j.contains("top_k")) j.at("top_k").get_to(r.top_k);
    if (j.contains("score_threshold")) j.at("score_threshold").get_to(r.score_threshold);
    if (j.contains("filter")) r.filter = j.at("filter");
}

struct SearchResult {
    std::string id;
    float score;
    std::string text;
    json metadata;
    std::string file_id;
};

inline void to_json(json& j, const SearchResult& r) {
    j = json{
        {"id", r.id},
        {"score", r.score},
        {"text", r.text},
        {"metadata", r.metadata}
    };
    if (r.file_id.empty()) {
        j["file_id"] = nullptr;
    } else {
        j["file_id"] = r.file_id;
    }
}

struct SearchResponse {
    std::vector<SearchResult> results;
    std::string retrieval_status; // "ok" | "no_match" | "below_threshold"
    float best_score_found = 0.0f;
    int32_t embedding_latency_ms = 0;
    int32_t search_latency_ms = 0;
};

inline void to_json(json& j, const SearchResponse& r) {
    j = json{
        {"results", r.results},
        {"retrieval_status", r.retrieval_status},
        {"best_score_found", r.best_score_found},
        {"embedding_latency_ms", r.embedding_latency_ms},
        {"search_latency_ms", r.search_latency_ms}
    };
}

} // namespace store
