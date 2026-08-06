// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// InferController — OIP v2 Inference Endpoints
//
// Implements the Open Inference Protocol v2 inference endpoints for both
// Predictive AI and Generative AI models.
//
// Routes:
//   POST /v2/models/{model}/infer            — Predictive AI tensor inference
//                                              (JSON or binary extension)
//   POST /v2/models/{model}/generate         — Generative AI blocking inference
//                                              (JSON or multipart for VLM)
//   POST /v2/models/{model}/generate_stream  — Generative AI SSE streaming
//   GET  /v2/health/ready                    — server ready check
//   GET  /v2/health/live                     — server live check
//
// Model discovery (GET /v2/models, GET /v2/models/{model}) is handled by
// ModelsController to keep routing concerns separate.
//
// Statelessness for generative models:
//   /generate and /generate_stream call ChatOrchestrator::resetKvCache()
//   before and after each inference to ensure clean Genie KV cache state.
//   This is the OIP stateless contract — each request is independent.
//
// Binary extension for /infer:
//   Content-Type: application/octet-stream
//   Inference-Header-Content-Length: <N>
//   Body: <N bytes JSON header><raw tensor bytes>
//
// Multipart for /generate (VLM images):
//   Content-Type: multipart/form-data
//   Parts: "request" (JSON) + "image_0", "image_1", ... (raw image bytes)
// ─────────────────────────────────────────────────────────────────────────────

#include <drogon/HttpController.h>
#include <string>

using namespace drogon;

class InferController : public drogon::HttpController<InferController> {
public:
    METHOD_LIST_BEGIN
        // POST /v2/models/{model}/infer — Predictive AI (JSON or binary extension)
        ADD_METHOD_TO(InferController::infer,
                      "/v2/models/{1}/infer", Post, Options);

        // POST /v2/models/{model}/generate — Generative AI blocking
        ADD_METHOD_TO(InferController::generate,
                      "/v2/models/{1}/generate", Post, Options);

        // POST /v2/models/{model}/generate_stream — Generative AI SSE streaming
        ADD_METHOD_TO(InferController::generateStream,
                      "/v2/models/{1}/generate_stream", Post, Options);

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
     * Runs Predictive AI tensor inference via PredictiveAIOrchestrator.
     * Supports both JSON and binary extension (Inference-Header-Content-Length).
     * Returns OIP v2 inference response.
     */
    void infer(const HttpRequestPtr& req,
               std::function<void(const HttpResponsePtr&)>&& callback,
               const std::string& model_name);

    /**
     * POST /v2/models/{model}/generate
     *
     * Runs Generative AI inference (blocking) via ChatOrchestrator.
     * Accepts:
     *   - JSON: { "text_input": "...", "parameters": {...} }
     *   - JSON: { "messages": [...], "parameters": {...} }  (server applies template)
     *   - Multipart: "request" part (JSON) + "image_N" parts (raw bytes, VLM only)
     *
     * Stateless: resets Genie KV cache before and after inference.
     */
    void generate(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback,
                  const std::string& model_name);

    /**
     * POST /v2/models/{model}/generate_stream
     *
     * Runs Generative AI inference with SSE streaming via ChatOrchestrator.
     * Same request format as /generate.
     * Response: text/event-stream with OipStreamChunk JSON per token.
     *
     * Stateless: resets Genie KV cache before and after inference.
     */
    void generateStream(const HttpRequestPtr& req,
                        std::function<void(const HttpResponsePtr&)>&& callback,
                        const std::string& model_name);

    /**
     * GET /v2/health/ready
     *
     * Returns 200 if the server has scanned model bundles and is ready.
     * Returns 503 if not ready.
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
