// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <drogon/HttpController.h>
#include <string>

using namespace drogon;

// ─────────────────────────────────────────────────────────────────────────────
// InferController — Layer 1 (KFServing v2 Inference Protocol)
//
// Implements the KFServing v2 / Open Inference Protocol for conventional AI
// models (classification, detection, segmentation).
//
// Routes:
//   POST /v2/models/{model}/infer   — run tensor inference
//   GET  /v2/models/{model}         — get model metadata (input/output specs)
//   GET  /v2/health/ready           — server ready check
//   GET  /v2/health/live            — server live check
//
// Responsibilities (Layer 1 only):
//   1. Parse KFServing v2 JSON request → TensorInferenceRequest (raw bytes)
//   2. Call IInferenceRouter::handleInfer() → TensorInferenceResponse
//   3. Format TensorInferenceResponse → KFServing v2 JSON response
//
// This layer MUST NOT contain any inference logic.
// All routing and inference is handled by qai-forge (Layers 2-4).
//
// KFServing v2 data encoding:
//   "data" field is a flat JSON array of numbers.
//   FP32 → each float stored as 4 bytes (little-endian).
//   INT8/UINT8 → each value stored as 1 byte.
//   INT32/UINT32 → each value stored as 4 bytes.
// ─────────────────────────────────────────────────────────────────────────────

class InferController : public drogon::HttpController<InferController> {
public:
    METHOD_LIST_BEGIN
        // POST /v2/models/{model}/infer — KFServing v2 inference
        ADD_METHOD_TO(InferController::infer,
                      "/v2/models/{1}/infer", Post, Options);

        // GET /v2/models/{model} — model metadata (input/output specs)
        ADD_METHOD_TO(InferController::getModelInfo,
                      "/v2/models/{1}", Get, Options);

        // GET /v2/health/ready — server ready (models loaded)
        ADD_METHOD_TO(InferController::healthReady,
                      "/v2/health/ready", Get, Options);

        // GET /v2/health/live — server live (process running)
        ADD_METHOD_TO(InferController::healthLive,
                      "/v2/health/live", Get, Options);
    METHOD_LIST_END

    /**
     * POST /v2/models/{model}/infer
     *
     * Parses KFServing v2 inference request, runs inference via
     * ConventionalAIOrchestrator, returns KFServing v2 response.
     */
    void infer(const HttpRequestPtr& req,
               std::function<void(const HttpResponsePtr&)>&& callback,
               const std::string& model_name);

    /**
     * GET /v2/models/{model}
     *
     * Returns model metadata: name, platform, input/output tensor specs.
     * Only available for conventional AI models (model_type == "conventional").
     */
    void getModelInfo(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback,
                      const std::string& model_name);

    /**
     * GET /v2/health/ready
     *
     * Returns 200 if the server has scanned model bundles and is ready
     * to serve requests. Returns 503 if not ready.
     */
    void healthReady(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback);

    /**
     * GET /v2/health/live
     *
     * Always returns 200 — the process is alive if it can respond.
     */
    void healthLive(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback);
};
