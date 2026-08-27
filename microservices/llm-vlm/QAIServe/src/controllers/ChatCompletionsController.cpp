// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "controllers/ChatCompletionsController.h"
#include "ChatCompletionStore.h"
#include "ChatCompletionUtils.h"
#include <qai_forge/QaiForge.h>
#include <qai_forge/InternalDTOs.h>
#include <qai_forge/managers/ModelConfigManager.h>
#include <drogon/drogon.h>
#include <nlohmann/json.hpp>

using json = nlohmann::ordered_json;
using namespace qai_forge;

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

ChatCompletionsController::ChatCompletionsController()
    : store_(std::make_unique<ChatCompletionStore>()) {
    LOG_INFO << "ChatCompletionsController initialized";
}

ChatCompletionsController::~ChatCompletionsController() {
    LOG_INFO << "ChatCompletionsController destroyed";
}

// ─────────────────────────────────────────────────────────────────────────────
// Error Response Formatting
// ─────────────────────────────────────────────────────────────────────────────

HttpResponsePtr ChatCompletionsController::formatErrorResponse(
    const std::string& message,
    const std::string& type,
    HttpStatusCode status
) {
    json error_body = {
        {"error", {
            {"message", message},
            {"type", type},
            {"code", nullptr}
        }}
    };

    auto resp = HttpResponse::newHttpResponse();
    resp->setBody(error_body.dump());
    resp->setContentTypeCode(CT_APPLICATION_JSON);
    resp->setStatusCode(status);
    resp->addHeader("Access-Control-Allow-Origin", "*");
    return resp;
}

// ─────────────────────────────────────────────────────────────────────────────
// POST /v1/chat/completions
// ─────────────────────────────────────────────────────────────────────────────

void ChatCompletionsController::createChatCompletion(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    // Handle CORS preflight
    if (req->method() == Options) {
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k204NoContent);
        resp->addHeader("Access-Control-Allow-Origin", "*");
        resp->addHeader("Access-Control-Allow-Methods", "POST, OPTIONS");
        resp->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
        callback(resp);
        return;
    }

    try {
        // Parse request body
        std::string body_str(req->body().data(), req->body().length());
        json body = json::parse(body_str);

        // Validate required fields
        if (!body.contains("model") || !body.contains("messages")) {
            callback(formatErrorResponse(
                "Missing required fields: model and messages",
                "invalid_request_error",
                k400BadRequest
            ));
            return;
        }

        const json& messages = body["messages"];
        if (!messages.is_array() || messages.empty()) {
            callback(formatErrorResponse(
                "messages must be a non-empty array",
                "invalid_request_error",
                k400BadRequest
            ));
            return;
        }

        std::string model = body["model"].get<std::string>();
        bool stream = body.value("stream", false);

        // Validate model exists
        auto& config_mgr = ModelConfigManager::getInstance();
        if (!config_mgr.validateModel(model)) {
            callback(formatErrorResponse(
                "Model '" + model + "' not found. Check /v1/models for available models.",
                "invalid_request_error",
                k404NotFound
            ));
            return;
        }

        // Extract optional user hint ID
        std::string user_hint_id;
        if (body.contains("id") && !body["id"].is_null()) {
            user_hint_id = body["id"].get<std::string>();
        }

        // Find or create session using hash-based lookup
        auto [session, is_new] = store_->findOrCreateSession(messages, user_hint_id);
        if (!session) {
            callback(formatErrorResponse(
                "Failed to create or find session",
                "internal_error",
                k500InternalServerError
            ));
            return;
        }

        // Update session model if not set
        if (session->model.empty()) {
            session->model = model;
        }

        LOG_INFO << "Chat completion request: session=" << session->completion_id
                 << " model=" << model << " stream=" << stream
                 << " is_new=" << is_new;

        // Prepare qai-forge request
        // For Chat Completions API (stateless), always pass the full conversation.
        // The orchestrator's execute() path creates a fresh ConversationSession
        // and processes the full messages array correctly (system in Slot 1,
        // history in Slot 4 if provided via response_history, current turn in Slot 5).
        CreateChatCompletionRequest forge_request;
        forge_request.model = model;
        forge_request.messages = messages;  // Full conversation from client
        forge_request.stream = stream;

        // Copy optional parameters
        if (body.contains("temperature") && !body["temperature"].is_null()) {
            forge_request.temperature = body["temperature"].get<float>();
        }
        if (body.contains("max_tokens") && !body["max_tokens"].is_null()) {
            forge_request.max_completion_tokens = body["max_tokens"].get<int>();
        }
        if (body.contains("top_p") && !body["top_p"].is_null()) {
            forge_request.top_p = body["top_p"].get<float>();
        }
        if (body.contains("top_k") && !body["top_k"].is_null()) {
            forge_request.top_k = body["top_k"].get<int>();
        }
        if (body.contains("frequency_penalty") && !body["frequency_penalty"].is_null()) {
            forge_request.frequency_penalty = body["frequency_penalty"].get<float>();
        }
        if (body.contains("presence_penalty") && !body["presence_penalty"].is_null()) {
            forge_request.presence_penalty = body["presence_penalty"].get<float>();
        }
        if (body.contains("tools") && !body["tools"].is_null()) {
            forge_request.tools = body["tools"];
        }
        if (body.contains("user") && !body["user"].is_null()) {
            forge_request.user = body["user"].get<std::string>();
        }

        // CRITICAL: Set user field to session ID to ensure isolated ConversationSession
        // in GenieOrchestrator. Without this, the orchestrator falls back to using
        // the model name as session ID, causing session collisions and "Failed to create dialog" errors.
        forge_request.user = session->completion_id;

        // Route to streaming or non-streaming handler
        if (stream) {
            handleStreamingRequest(session, forge_request, std::move(callback));
        } else {
            handleNonStreamingRequest(session, forge_request, std::move(callback));
        }

    } catch (const json::exception& e) {
        LOG_ERROR << "JSON parse error in createChatCompletion: " << e.what();
        callback(formatErrorResponse(
            std::string("Invalid JSON: ") + e.what(),
            "invalid_request_error",
            k400BadRequest
        ));
    } catch (const std::exception& e) {
        LOG_ERROR << "Exception in createChatCompletion: " << e.what();
        callback(formatErrorResponse(
            std::string("Internal error: ") + e.what(),
            "internal_error",
            k500InternalServerError
        ));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Streaming Handler
// ─────────────────────────────────────────────────────────────────────────────

void ChatCompletionsController::handleStreamingRequest(
    ChatSession* session,
    const CreateChatCompletionRequest& request,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    auto stream_resp = HttpResponse::newAsyncStreamResponse(
        [session, request, this](drogon::ResponseStreamPtr stream_ptr) {
            // Convert to shared_ptr for lambda capture
            auto stream = std::shared_ptr<drogon::ResponseStream>(std::move(stream_ptr));

            // Setup qai-forge callbacks
            StreamCallbacks callbacks;

            // Accumulated content for session update
            auto accumulated_content = std::make_shared<std::string>();
            auto accumulated_tool_calls = std::make_shared<json>();
            auto has_tool_calls = std::make_shared<bool>(false);

            callbacks.onToken = [stream, session, accumulated_content](const StreamChunk& chunk) {
                // Accumulate content
                if (chunk.content_delta) {
                    *accumulated_content += *chunk.content_delta;
                }

                // Format as OpenAI chat completion chunk
                json chunk_json = {
                    {"id", session->completion_id},
                    {"object", "chat.completion.chunk"},
                    {"created", std::time(nullptr)},
                    {"model", session->model},
                    {"choices", json::array({
                        {
                            {"index", 0},
                            {"delta", {
                                {"content", chunk.content_delta.value_or("")}
                            }},
                            {"finish_reason", nullptr}
                        }
                    })}
                };

                // Send as SSE
                std::string sse = "data: " + chunk_json.dump() + "\n\n";
                stream->send(sse);
            };

            callbacks.onComplete = [stream, session, accumulated_content, accumulated_tool_calls, has_tool_calls, this](
                const StandardResponse& final_response
            ) {
                // Use final response content if available
                if (final_response.content) {
                    *accumulated_content = *final_response.content;
                }

                // Build assistant message
                json assistant_msg = {
                    {"role", "assistant"},
                    {"content", *accumulated_content}
                };

                // Handle tool calls
                if (final_response.tool_calls && !final_response.tool_calls->empty()) {
                    assistant_msg["tool_calls"] = *final_response.tool_calls;
                    *has_tool_calls = true;

                    // Update tool call state
                    session->has_active_tool_call = true;
                    session->tool_call_timestamp = std::chrono::steady_clock::now();

                    // Calculate tool call hash
                    json messages_with_tool_call = session->messages;
                    messages_with_tool_call.push_back(assistant_msg);
                    session->tool_call_hash = ChatCompletionUtils::hashSpecificMessages(messages_with_tool_call);
                }

                // Update session
                session->messages.push_back(assistant_msg);
                store_->updateSession(session->completion_id, session->messages);

                // Send final chunk with finish_reason
                json final_chunk = {
                    {"id", session->completion_id},
                    {"object", "chat.completion.chunk"},
                    {"created", std::time(nullptr)},
                    {"model", session->model},
                    {"choices", json::array({
                        {
                            {"index", 0},
                            {"delta", json::object()},
                            {"finish_reason", *has_tool_calls ? "tool_calls" : "stop"}
                        }
                    })}
                };

                std::string final_sse = "data: " + final_chunk.dump() + "\n\n";
                stream->send(final_sse);

                std::string done_sse = "data: [DONE]\n\n";
                stream->send(done_sse);
                stream->close();

                LOG_INFO << "Streaming completed for session " << session->completion_id;
            };

            callbacks.onError = [stream, session](const GenAIException& error) {
                LOG_ERROR << "Streaming error for session " << session->completion_id
                          << ": " << error.message;

                json error_chunk = {
                    {"error", {
                        {"message", error.message},
                        {"type", "server_error"}
                    }}
                };

                std::string error_sse = "data: " + error_chunk.dump() + "\n\n";
                stream->send(error_sse);
                stream->close();
            };

            callbacks.onCancelled = [stream, session]() {
                LOG_INFO << "Streaming cancelled for session " << session->completion_id;
                stream->close();
            };

            // Start generation
            try {
                GenerateOptions options;
                options.response_id = session->completion_id;
                options.session_id = session->completion_id;
                // No response_history needed - full conversation is in request.messages

                // Detect tool output submission
                bool has_tool_response = false;
                for (const auto& msg : request.messages) {
                    if (msg.is_object() && msg.value("role", "") == "tool") {
                        has_tool_response = true;
                        break;
                    }
                }
                if (has_tool_response && !session->messages.empty()) {
                    options.tool_output_submission = true;
                    options.allow_tool_chain_fallback = true;
                }

                qai_forge::QaiForge::getInstance().generateStream(
                    request,
                    callbacks,
                    options
                );

            } catch (const GenAIException& e) {
                callbacks.onError(e);
            } catch (const std::exception& e) {
                GenAIException error(
                    GenAIErrorCode::INTERNAL_ERROR,
                    e.what(),
                    500
                );
                callbacks.onError(error);
            }
        }
    );

    // Set headers
    stream_resp->setStatusCode(k200OK);
    stream_resp->addHeader("Content-Type", "text/event-stream");
    stream_resp->addHeader("Cache-Control", "no-cache");
    stream_resp->addHeader("Connection", "keep-alive");
    stream_resp->addHeader("Access-Control-Allow-Origin", "*");

    callback(stream_resp);
}

// ─────────────────────────────────────────────────────────────────────────────
// Non-Streaming Handler
// ─────────────────────────────────────────────────────────────────────────────

void ChatCompletionsController::handleNonStreamingRequest(
    ChatSession* session,
    const CreateChatCompletionRequest& request,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    try {
        // Prepare options
        GenerateOptions options;
        options.response_id = session->completion_id;
        options.session_id = session->completion_id;
        // No response_history needed - full conversation is in request.messages

        // Detect tool output submission
        bool has_tool_response = false;
        for (const auto& msg : request.messages) {
            if (msg.is_object() && msg.value("role", "") == "tool") {
                has_tool_response = true;
                break;
            }
        }
        if (has_tool_response && !session->messages.empty()) {
            options.tool_output_submission = true;
            options.allow_tool_chain_fallback = true;
        }

        // Synchronous generation
        auto response = qai_forge::QaiForge::getInstance().generate(request, options);

        // Build assistant message
        json assistant_msg = {
            {"role", "assistant"}
        };

        if (response.content) {
            assistant_msg["content"] = *response.content;
        } else {
            assistant_msg["content"] = "";
        }

        // Handle tool calls
        bool has_tool_calls = false;
        if (response.tool_calls && !response.tool_calls->empty()) {
            assistant_msg["tool_calls"] = *response.tool_calls;
            has_tool_calls = true;

            // Update tool call state
            session->has_active_tool_call = true;
            session->tool_call_timestamp = std::chrono::steady_clock::now();

            // Calculate tool call hash
            json messages_with_tool_call = session->messages;
            messages_with_tool_call.push_back(assistant_msg);
            session->tool_call_hash = ChatCompletionUtils::hashSpecificMessages(messages_with_tool_call);
        }

        // Update session
        session->messages.push_back(assistant_msg);
        store_->updateSession(session->completion_id, session->messages);

        // Build usage object separately to avoid nested initializer issues
        json usage_obj = {
            {"prompt_tokens", response.prompt_tokens},
            {"completion_tokens", response.completion_tokens},
            {"total_tokens", response.total_tokens}
        };

        // Format OpenAI-compatible response
        json response_json = {
            {"id", session->completion_id},
            {"object", "chat.completion"},
            {"created", std::time(nullptr)},
            {"model", session->model},
            {"choices", json::array({
                {
                    {"index", 0},
                    {"message", assistant_msg},
                    {"finish_reason", has_tool_calls ? "tool_calls" : "stop"}
                }
            })},
            {"usage", usage_obj}
        };

        auto resp = HttpResponse::newHttpResponse();
        resp->setBody(response_json.dump());
        resp->setContentTypeCode(CT_APPLICATION_JSON);
        resp->addHeader("Access-Control-Allow-Origin", "*");
        callback(resp);

        LOG_INFO << "Non-streaming completed for session " << session->completion_id;

    } catch (const GenAIException& e) {
        LOG_ERROR << "Generation error for session " << session->completion_id
                  << ": " << e.message;

        callback(formatErrorResponse(
            e.message,
            "server_error",
            static_cast<HttpStatusCode>(e.http_status)
        ));

    } catch (const std::exception& e) {
        LOG_ERROR << "Exception in handleNonStreamingRequest: " << e.what();

        callback(formatErrorResponse(
            std::string("Internal error: ") + e.what(),
            "internal_error",
            k500InternalServerError
        ));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// DELETE /v1/chat/completions/{id}
// ─────────────────────────────────────────────────────────────────────────────

void ChatCompletionsController::deleteChatCompletion(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    const std::string& completion_id
) {
    // Handle CORS preflight
    if (req->method() == Options) {
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k204NoContent);
        resp->addHeader("Access-Control-Allow-Origin", "*");
        resp->addHeader("Access-Control-Allow-Methods", "DELETE, OPTIONS");
        resp->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
        callback(resp);
        return;
    }

    try {
        // Find session
        auto session = store_->findByCompletionId(completion_id);
        if (!session) {
            callback(formatErrorResponse(
                "Session not found",
                "not_found_error",
                k404NotFound
            ));
            return;
        }

        // Cancel active job if any
        if (!session->active_job_id.empty()) {
            qai_forge::QaiForge::getInstance().cancel(session->active_job_id);
        }

        // Delete session
        store_->deleteSession(completion_id);

        // Return success
        json response_json = {
            {"id", completion_id},
            {"object", "chat.completion"},
            {"deleted", true}
        };

        auto resp = HttpResponse::newHttpResponse();
        resp->setBody(response_json.dump());
        resp->setContentTypeCode(CT_APPLICATION_JSON);
        resp->addHeader("Access-Control-Allow-Origin", "*");
        callback(resp);

        LOG_INFO << "Deleted session " << completion_id;

    } catch (const std::exception& e) {
        LOG_ERROR << "Exception in deleteChatCompletion: " << e.what();

        callback(formatErrorResponse(
            std::string("Internal error: ") + e.what(),
            "internal_error",
            k500InternalServerError
        ));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// POST /v1/cancel/{id}
// ─────────────────────────────────────────────────────────────────────────────

void ChatCompletionsController::cancelChatCompletion(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    const std::string& completion_id
) {
    // Handle CORS preflight
    if (req->method() == Options) {
        auto resp = HttpResponse::newHttpResponse();
        resp->setStatusCode(k204NoContent);
        resp->addHeader("Access-Control-Allow-Origin", "*");
        resp->addHeader("Access-Control-Allow-Methods", "POST, OPTIONS");
        resp->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
        callback(resp);
        return;
    }

    try {
        // Find session
        auto session = store_->findByCompletionId(completion_id);
        if (!session) {
            callback(formatErrorResponse(
                "Session not found",
                "not_found_error",
                k404NotFound
            ));
            return;
        }

        // Cancel active job
        bool cancelled = false;
        if (!session->active_job_id.empty()) {
            cancelled = qai_forge::QaiForge::getInstance().cancel(session->active_job_id);
            if (cancelled) {
                session->active_job_id.clear();
                store_->updateSession(completion_id, session->messages);
            }
        }

        // Return result
        json response_json = {
            {"id", completion_id},
            {"object", "chat.completion"},
            {"cancelled", cancelled}
        };

        auto resp = HttpResponse::newHttpResponse();
        resp->setBody(response_json.dump());
        resp->setContentTypeCode(CT_APPLICATION_JSON);
        resp->addHeader("Access-Control-Allow-Origin", "*");
        callback(resp);

        LOG_INFO << "Cancel request for session " << completion_id
                 << ": " << (cancelled ? "success" : "no active job");

    } catch (const std::exception& e) {
        LOG_ERROR << "Exception in cancelChatCompletion: " << e.what();

        callback(formatErrorResponse(
            std::string("Internal error: ") + e.what(),
            "internal_error",
            k500InternalServerError
        ));
    }
}
