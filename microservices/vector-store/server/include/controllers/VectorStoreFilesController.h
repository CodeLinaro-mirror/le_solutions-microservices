//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#pragma once

#include <drogon/HttpController.h>
#include <nlohmann/json.hpp>
#include "store/VectorStoreManager.h"

namespace controllers {

using json = nlohmann::json;

/**
 * @brief REST Controller handling asynchronous file ingestion and job progress tracking.
 */
class VectorStoreFilesController : public drogon::HttpController<VectorStoreFilesController> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(VectorStoreFilesController::ingestFiles, "/v1/vector_stores/{id}/files", drogon::Post);
        ADD_METHOD_TO(VectorStoreFilesController::insertWithVectors, "/v1/vector_stores/{id}/insert_with_vectors", drogon::Post);
        ADD_METHOD_TO(VectorStoreFilesController::getDocument, "/v1/vector_stores/{id}/documents/{doc_id}", drogon::Get);
        ADD_METHOD_TO(VectorStoreFilesController::getJobStatus, "/v1/vector_stores/{id}/files/{job_id}", drogon::Get);
        ADD_METHOD_TO(VectorStoreFilesController::listJobs, "/v1/vector_stores/{id}/files", drogon::Get);
    METHOD_LIST_END

    void ingestFiles(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     std::string id);

    void insertWithVectors(const drogon::HttpRequestPtr& req,
                           std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                           std::string id);

    void getDocument(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     std::string id,
                     std::string doc_id);

    void getJobStatus(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                      std::string id,
                      std::string job_id);

    void listJobs(const drogon::HttpRequestPtr& req,
                  std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                  std::string id);

private:
    store::VectorStoreManager& manager_ = store::VectorStoreManager::getInstance();
};

} // namespace controllers
