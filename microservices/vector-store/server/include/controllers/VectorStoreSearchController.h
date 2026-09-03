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
 * @brief REST Controller handling semantic searches over a vector store under /v1/vector_stores/{id}/search
 */
class VectorStoreSearchController : public drogon::HttpController<VectorStoreSearchController> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(VectorStoreSearchController::searchStore, "/v1/vector_stores/{id}/search", drogon::Post);
    METHOD_LIST_END

    void searchStore(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     std::string id);

private:
    store::VectorStoreManager& manager_ = store::VectorStoreManager::getInstance();
};

} // namespace controllers
