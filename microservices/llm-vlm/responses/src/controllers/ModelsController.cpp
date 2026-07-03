// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// ModelsController — OpenAI-compatible model discovery for the Responses service
// ─────────────────────────────────────────────────────────────────────────────

#include "controllers/ModelsController.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include <drogon/HttpResponse.h>
#include <nlohmann/json.hpp>
#include <ctime>

using json = nlohmann::ordered_json;

namespace {

HttpResponsePtr jsonResp(const json& body, HttpStatusCode code = k200OK) {
    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(code);
    resp->setContentTypeCode(CT_APPLICATION_JSON);
    resp->setBody(body.dump());
    return resp;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// GET /v1/models — OpenAI-compatible model list (generative models only)
// ─────────────────────────────────────────────────────────────────────────────
void ModelsController::listModels(
    const HttpRequestPtr& /*req*/,
    std::function<void(const HttpResponsePtr&)>&& callback) {

    auto models = ModelConfigManager::getInstance().getAvailableModels();

    json data = json::array();
    for (const auto& m : models) {
        // Responses API only serves generative models
        if (m.model_type != "generative") continue;
        data.push_back({
            {"id",       m.id},
            {"object",   "model"},
            {"created",  static_cast<int64_t>(std::time(nullptr))},
            {"owned_by", ""},
        });
    }

    callback(jsonResp({
        {"object", "list"},
        {"data",   data},
    }));
}

// ─────────────────────────────────────────────────────────────────────────────
// GET /v1/health — Health check
// ─────────────────────────────────────────────────────────────────────────────
void ModelsController::health(
    const HttpRequestPtr& /*req*/,
    std::function<void(const HttpResponsePtr&)>&& callback) {

    callback(jsonResp({{"status", "healthy"}}));
}
