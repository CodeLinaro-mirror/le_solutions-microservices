//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#include "controllers/VectorStoreFilesController.h"

namespace controllers {

void VectorStoreFilesController::ingestFiles(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id
) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);

    try {
        json body = json::parse(req->body());
        store::IngestDocumentsRequest f_req = body.get<store::IngestDocumentsRequest>();

        // Enqueue document ingestion asynchronously
        std::string job_id = manager_.enqueueIngestion(id, f_req.file_id, std::move(f_req.chunks));

        resp->setBody(json{
            {"job_id", job_id},
            {"status", "processing"},
            {"message", "File ingestion initiated successfully."}
        }.dump());
        resp->setStatusCode(drogon::k202Accepted);

    } catch (const std::exception& e) {
        resp->setBody(json{{"error", e.what()}}.dump());
        resp->setStatusCode(drogon::k400BadRequest);
    }

    callback(resp);
}

void VectorStoreFilesController::insertWithVectors(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id
) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);

    try {
        json body = json::parse(req->body());
        std::string file_id = body.value("file_id", "manual_insertion");

        std::vector<store::DocumentChunk> chunks = body.at("chunks").get<std::vector<store::DocumentChunk>>();
        std::vector<std::vector<float>> embeddings = body.at("embeddings").get<std::vector<std::vector<float>>>();

        if (chunks.size() != embeddings.size()) {
            throw std::invalid_argument("Chunks array and embeddings array size mismatch");
        }

        // Pad/resize vectors to exactly 768 to prevent pgvector dimension mismatch issues
        for (auto& vec : embeddings) {
            if (vec.size() != 768) {
                vec.resize(768, 0.0f);
            }
        }

        // Insert documents with pre-computed vectors directly into PostgreSQL repository
        manager_.repository().insertDocuments(id, file_id, chunks, embeddings);

        resp->setBody(json{
            {"status", "completed"},
            {"inserted_count", chunks.size()},
            {"message", "Documents and pre-computed embeddings saved directly and successfully."}
        }.dump());
        resp->setStatusCode(drogon::k201Created);

    } catch (const std::exception& e) {
        resp->setBody(json{{"error", e.what()}}.dump());
        resp->setStatusCode(drogon::k400BadRequest);
    }

    callback(resp);
}

void VectorStoreFilesController::getDocument(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id,
    std::string doc_id
) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);

    try {
        auto doc = manager_.repository().getDocumentWithEmbedding(id, doc_id);

        json j;
        store::to_json(j, doc);

        resp->setBody(j.dump());
        resp->setStatusCode(drogon::k200OK);

    } catch (const std::exception& e) {
        resp->setBody(json{{"error", e.what()}}.dump());
        resp->setStatusCode(drogon::k404NotFound);
    }

    callback(resp);
}

void VectorStoreFilesController::getJobStatus(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id,
    std::string job_id
) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);

    try {
        auto job = manager_.repository().getJob(job_id);

        json j_json;
        store::to_json(j_json, job);

        resp->setBody(j_json.dump());
        resp->setStatusCode(drogon::k200OK);

    } catch (const std::exception& e) {
        resp->setBody(json{{"error", e.what()}}.dump());
        resp->setStatusCode(drogon::k404NotFound);
    }

    callback(resp);
}

void VectorStoreFilesController::listJobs(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback,
    std::string id
) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);

    try {
        auto jobs = manager_.repository().listJobs(id);

        json j_list = json::array();
        for (const auto& job : jobs) {
            json j_json;
            store::to_json(j_json, job);
            j_list.push_back(j_json);
        }

        resp->setBody(json{{"jobs", j_list}}.dump());
        resp->setStatusCode(drogon::k200OK);

    } catch (const std::exception& e) {
        resp->setBody(json{{"error", e.what()}}.dump());
        resp->setStatusCode(drogon::k500InternalServerError);
    }

    callback(resp);
}

} // namespace controllers
