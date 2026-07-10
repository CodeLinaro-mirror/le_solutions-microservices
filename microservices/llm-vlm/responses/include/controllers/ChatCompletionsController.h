// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <drogon/HttpController.h>
#include <memory>
#include <string>

using namespace drogon;

// Forward declarations
class ChatCompletionStore;
struct ChatSession;

// ─────────────────────────────────────────────────────────────────────────────
// ChatCompletionsController — OpenAI Chat Completions API
//
// Implements the OpenAI Chat Completions API for backward compatibility
// with applications currently using the chatcompletions service:
//   POST   /v1/chat/completions           — Create chat completion
//   DELETE /v1/chat/completions/{id}      — Delete session
//   POST   /v1/cancel/{id}                — Cancel active job
//
// Key features:
//   - Hash-based session identification (stateless, idempotent)
//   - Automatic slot-based summarization via qai-forge ConversationSession
//   - Streaming and non-streaming support
//   - Tool calling (2-trip pattern)
//   - Full OpenAI API compatibility
//
// This controller uses the qai-forge inference facade and integrates with
// the existing responses container infrastructure.
// ─────────────────────────────────────────────────────────────────────────────

class ChatCompletionsController : public drogon::HttpController<ChatCompletionsController> {
public:
    METHOD_LIST_BEGIN
        // POST /v1/chat/completions
        ADD_METHOD_TO(ChatCompletionsController::createChatCompletion,
                      "/v1/chat/completions", Post, Options);

        // DELETE /v1/chat/completions/{id}
        ADD_METHOD_TO(ChatCompletionsController::deleteChatCompletion,
                      "/v1/chat/completions/{1}", Delete, Options);

        // POST /v1/cancel/{id}
        ADD_METHOD_TO(ChatCompletionsController::cancelChatCompletion,
                      "/v1/cancel/{1}", Post, Options);
    METHOD_LIST_END

    ChatCompletionsController();
    ~ChatCompletionsController();

    /**
     * @brief Create a chat completion (streaming or non-streaming)
     *
     * Implements the OpenAI Chat Completions API endpoint.
     * Supports hash-based session lookup, automatic summarization,
     * tool calling, and both streaming and non-streaming modes.
     *
     * @param req HTTP request with JSON body
     * @param callback Response callback
     */
    void createChatCompletion(
        const HttpRequestPtr& req,
        std::function<void(const HttpResponsePtr&)>&& callback);

    /**
     * @brief Delete a chat completion session
     *
     * Removes the session from the store and cancels any active job.
     *
     * @param req HTTP request
     * @param callback Response callback
     * @param completion_id Session ID to delete
     */
    void deleteChatCompletion(
        const HttpRequestPtr& req,
        std::function<void(const HttpResponsePtr&)>&& callback,
        const std::string& completion_id);

    /**
     * @brief Cancel an active chat completion job
     *
     * Cancels the qai-forge job associated with the session.
     *
     * @param req HTTP request
     * @param callback Response callback
     * @param completion_id Session ID to cancel
     */
    void cancelChatCompletion(
        const HttpRequestPtr& req,
        std::function<void(const HttpResponsePtr&)>&& callback,
        const std::string& completion_id);

private:
    /**
     * @brief Handle streaming chat completion request
     *
     * Sets up Drogon AsyncStreamResponse with qai-forge StreamCallbacks.
     * Formats output as Server-Sent Events (SSE).
     *
     * @param session Session pointer
     * @param request qai-forge request
     * @param callback Response callback
     */
    void handleStreamingRequest(
        struct ChatSession* session,
        const struct CreateChatCompletionRequest& request,
        std::function<void(const HttpResponsePtr&)>&& callback);

    /**
     * @brief Handle non-streaming chat completion request
     *
     * Calls qai-forge synchronously and returns complete response.
     *
     * @param session Session pointer
     * @param request qai-forge request
     * @param callback Response callback
     */
    void handleNonStreamingRequest(
        struct ChatSession* session,
        const struct CreateChatCompletionRequest& request,
        std::function<void(const HttpResponsePtr&)>&& callback);

    /**
     * @brief Format error response
     *
     * Creates OpenAI-compatible error response.
     *
     * @param message Error message
     * @param type Error type (e.g., "invalid_request_error")
     * @param status HTTP status code
     * @return HTTP response
     */
    HttpResponsePtr formatErrorResponse(
        const std::string& message,
        const std::string& type,
        HttpStatusCode status);

    // Session store (hash-based lookup)
    std::unique_ptr<ChatCompletionStore> store_;
};
