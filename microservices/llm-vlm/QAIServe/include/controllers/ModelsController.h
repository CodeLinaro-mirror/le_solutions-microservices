// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// ModelsController — Unified model discovery endpoints
//
// Routes:
//   GET /v1/models              — OpenAI-compatible model list (generative only)
//   GET /v2/models              — OIP model list (all models: predictive + generative)
//   GET /v2/models/{model_id}   — OIP model metadata with tensor specs
//   GET /v2/postprocesses       — Postprocessing plugins catalog
// ─────────────────────────────────────────────────────────────────────────────

#include <drogon/HttpController.h>
#include <string>

using namespace drogon;

class ModelsController : public drogon::HttpController<ModelsController> {
public:
    METHOD_LIST_BEGIN
        // GET /v1/models — OpenAI-compatible (generative models only)
        ADD_METHOD_TO(ModelsController::listModelsV1,
                      "/v1/models", Get, Options);

        // GET /v2/models — OIP (all models)
        ADD_METHOD_TO(ModelsController::listModelsV2,
                      "/v2/models", Get, Options);

        // GET /v2/models/{model_id} — OIP metadata
        ADD_METHOD_TO(ModelsController::getModelV2,
                      "/v2/models/{1}", Get, Options);

        // GET /v2/postprocesses — postprocessing plugins catalog
        ADD_METHOD_TO(ModelsController::listPostprocesses,
                      "/v2/postprocesses", Get, Options);
    METHOD_LIST_END

    /**
     * GET /v1/models
     *
     * Returns OpenAI-compatible model list (generative models only).
     * Format: { "object": "list", "data": [{ "id": "...", "object": "model", ... }] }
     */
    void listModelsV1(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback);

    /**
     * GET /v2/models
     *
     * Returns OIP model list — ALL installed models (predictive + generative).
     * Format: [{ "name": "...", "model_type": "...", "runtime": "...", ... }]
     */
    void listModelsV2(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback);

    /**
     * GET /v2/models/{model_id}
     *
     * Returns full OIP metadata for a single model, including:
     * - model_type, runtime, precision
     * - inputs/outputs tensor specs (from metadata.json model_files)
     * - chat_template (for generative models — clients can use for prompt formatting)
     * - supports_vision, context_size (for generative models)
     */
    void getModelV2(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback,
                    const std::string& model_id);

    /**
     * GET /v2/postprocesses
     *
     * Returns the full postprocess plugin catalog: every registered
     * postprocess's name, description, supported tensor layouts
     * and accepted query parameters.
     */
    void listPostprocesses(const HttpRequestPtr& req,
                            std::function<void(const HttpResponsePtr&)>&& callback);
};
