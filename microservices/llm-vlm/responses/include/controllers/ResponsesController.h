// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <drogon/HttpController.h>
#include <string>

using namespace drogon;

// ─────────────────────────────────────────────────────────────────────────────
// ResponsesController — Layer 1 (Drogon HTTP Adapter for Responses API)
//
// Implements the OpenAI Responses API (limited on-device subset):
//   POST   /v1/responses                    — Create a response
//   GET    /v1/responses/{response_id}      — Retrieve a stored response
//   DELETE /v1/responses/{response_id}      — Delete a response
//   POST   /v1/responses/{response_id}/cancel — Cancel an active response
//
// Key differences from Chat Completions:
//   - Input: `input` field (string or array) instead of `messages[]`
//   - Stateful: `previous_response_id` links turns (server manages history)
//   - Output: `output[]` array with typed items (message, tool_call, etc.)
//   - Built-in tools: web_search returns "not available on-device" gracefully
//
// This controller calls the model scheduler and formats
// the results into the Responses API wire format.
// ─────────────────────────────────────────────────────────────────────────────

class ResponsesController : public drogon::HttpController<ResponsesController> {
public:
    METHOD_LIST_BEGIN
        // ADD_METHOD_TO = absolute path, bypasses the class-name-derived prefix
        // POST /v1/responses
        ADD_METHOD_TO(ResponsesController::createResponse, "/v1/responses", Post, Options);

        // GET /v1/responses/{response_id}
        ADD_METHOD_TO(ResponsesController::getResponse, "/v1/responses/{1}", Get, Options);

        // GET /v1/responses/{response_id}/input_items
        ADD_METHOD_TO(ResponsesController::listInputItems, "/v1/responses/{1}/input_items", Get, Options);

        // DELETE /v1/responses/{response_id}
        ADD_METHOD_TO(ResponsesController::deleteResponse, "/v1/responses/{1}", Delete, Options);

        // POST /v1/responses/{response_id}/cancel
        ADD_METHOD_TO(ResponsesController::cancelResponse, "/v1/responses/{1}/cancel", Post, Options);
    METHOD_LIST_END

    void createResponse(const HttpRequestPtr& req,
                        std::function<void(const HttpResponsePtr&)>&& callback);

    void getResponse(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback,
                     const std::string& response_id);

    void listInputItems(const HttpRequestPtr& req,
                        std::function<void(const HttpResponsePtr&)>&& callback,
                        const std::string& response_id);

    void deleteResponse(const HttpRequestPtr& req,
                        std::function<void(const HttpResponsePtr&)>&& callback,
                        const std::string& response_id);

    void cancelResponse(const HttpRequestPtr& req,
                        std::function<void(const HttpResponsePtr&)>&& callback,
                        const std::string& response_id);
};
