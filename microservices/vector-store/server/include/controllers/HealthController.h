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
 * @brief REST Controller handling platform health-check snapshot API endpoints.
 */
class HealthController : public drogon::HttpController<HealthController> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(HealthController::getHealth, "/v1/health", drogon::Get);
        ADD_METHOD_TO(HealthController::getStoreHealth, "/v1/vector_stores/{id}/health", drogon::Get);
    METHOD_LIST_END

    void getHealth(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void getStoreHealth(const drogon::HttpRequestPtr& req,
                        std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                        std::string id);

private:
    store::VectorStoreManager& manager_ = store::VectorStoreManager::getInstance();
};

} // namespace controllers
