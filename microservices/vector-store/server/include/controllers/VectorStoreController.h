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
 * @brief REST Controller handling vector store CRUD operations under /v1/vector_stores
 */
class VectorStoreController : public drogon::HttpController<VectorStoreController> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(VectorStoreController::createStore, "/v1/vector_stores", drogon::Post);
        ADD_METHOD_TO(VectorStoreController::listStores, "/v1/vector_stores", drogon::Get);
        ADD_METHOD_TO(VectorStoreController::getStore, "/v1/vector_stores/{id}", drogon::Get);
        ADD_METHOD_TO(VectorStoreController::updateStore, "/v1/vector_stores/{id}", drogon::Put, drogon::Patch);
        ADD_METHOD_TO(VectorStoreController::deleteStore, "/v1/vector_stores/{id}", drogon::Delete);
    METHOD_LIST_END

    void createStore(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void listStores(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void getStore(const drogon::HttpRequestPtr& req,
                  std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                  std::string id);

    void updateStore(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     std::string id);

    void deleteStore(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     std::string id);

private:
    store::VectorStoreManager& manager_ = store::VectorStoreManager::getInstance();
};

} // namespace controllers
