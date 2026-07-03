// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// ModelsController — OpenAI-compatible model discovery
//
// Routes:
//   GET /v1/models  — OpenAI-compatible model list (generative models only)
//   GET /v1/health  — Health check
// ─────────────────────────────────────────────────────────────────────────────

#include <drogon/HttpController.h>

using namespace drogon;

class ModelsController : public drogon::HttpController<ModelsController> {
public:
    METHOD_LIST_BEGIN
        // GET /v1/models — OpenAI-compatible model list
        ADD_METHOD_TO(ModelsController::listModels, "/v1/models", Get, Options);

        // GET /v1/health — Health check
        ADD_METHOD_TO(ModelsController::health, "/v1/health", Get, Options);
    METHOD_LIST_END

    /**
     * GET /v1/models
     *
     * Returns OpenAI-compatible model list (generative models only).
     * Format: { "object": "list", "data": [{ "id": "...", "object": "model", ... }] }
     */
    void listModels(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback);

    /**
     * GET /v1/health
     *
     * Returns 200 if the server has scanned model bundles and is ready.
     */
    void health(const HttpRequestPtr& req,
                std::function<void(const HttpResponsePtr&)>&& callback);
};
