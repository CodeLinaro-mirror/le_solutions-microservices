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

namespace {

constexpr const char* kChatMemoryNamespace = "responses.chat";

std::string getStringOrDefault(const json& object,
                               const std::string& key,
                               const std::string& default_value = "") {
    if (!object.is_object() || !object.contains(key)
        || object[key].is_null() || !object[key].is_string()) {
        return default_value;
    }
    return object[key].get<std::string>();
}

GenerateOptions makeChatGenerateOptions(const BeginChatTurnResult& turn,
                                        const json& messages) {
    GenerateOptions options;
    options.response_id = turn.completion_id;
    options.session_id = turn.completion_id;

    ConversationReference reference;
    reference.namespace_id = kChatMemoryNamespace;
    reference.conversation_id = turn.completion_id;
    reference.turn_id = turn.turn_id;
    if (!turn.parent_turn_id.empty()) {
        reference.parent_policy = ConversationParentPolicy::Explicit;
        reference.parent_turn_id = turn.parent_turn_id;
    } else {
        reference.parent_policy = ConversationParentPolicy::Root;
    }
    options.conversation = std::move(reference);

    for (const auto& message : messages) {
        if (getStringOrDefault(message, "role") == "tool") {
            options.previous_response_id = turn.completion_id;
            options.tool_output_submission = true;
            options.allow_tool_chain_fallback = true;
            break;
        }
    }
    return options;
}

HttpResponsePtr buildReplayResponse(const BeginChatTurnResult& turn,
                                    const json& replay_result) {
    json assistant_message = {
        {"role", "assistant"},
        {"content", getStringOrDefault(replay_result, "content")}
    };
    if (replay_result.contains("tool_calls")
        && replay_result["tool_calls"].is_array()
        && !replay_result["tool_calls"].empty()) {
        assistant_message["tool_calls"] = replay_result["tool_calls"];
    }

    json response_json = {
        {"id", turn.completion_id},
        {"object", "chat.completion"},
        {"created", std::time(nullptr)},
        {"model", turn.model},
        {"choices", json::array({{
            {"index", 0},
            {"message", assistant_message},
            {"finish_reason", replay_result.value("finish_reason", "stop")}
        }})},
        {"usage", {
            {"prompt_tokens", replay_result.value("prompt_tokens", 0)},
            {"completion_tokens", replay_result.value("completion_tokens", 0)},
            {"total_tokens", replay_result.value("total_tokens", 0)}
        }}
    };
    auto response = HttpResponse::newHttpResponse();
    response->setBody(response_json.dump());
    response->setContentTypeCode(CT_APPLICATION_JSON);
    response->addHeader("Access-Control-Allow-Origin", "*");
    return response;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────

ChatCompletionsController::ChatCompletionsController()
    : store_(std::make_unique<ChatCompletionStore>()) {
    LOG_INFO << "ChatCompletionsController initialized";
}

ChatCompletionsController::~ChatCompletionsController() {
    for (const std::string& completion_id : store_->getAllSessionIds()) {
        QaiForge::getInstance().releaseConversation(
            kChatMemoryNamespace, completion_id);
    }
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

    std::optional<BeginChatTurnResult> active_turn;
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

        BeginChatTurnResult turn =
            store_->beginTurn(messages, body, model, user_hint_id);
        if (!turn.ok) {
            callback(formatErrorResponse(
                turn.error_message.empty()
                    ? "Failed to create or find session"
                    : turn.error_message,
                "invalid_request_error",
                k409Conflict
            ));
            return;
        }
        if (turn.replay_result.has_value()) {
            callback(buildReplayResponse(turn, turn.replay_result.value()));
            return;
        }
        active_turn = turn;

        LOG_INFO << "Chat completion request: session=" << turn.completion_id
                 << " model=" << model << " stream=" << stream
                 << " is_new=" << turn.is_new;

        // The client-supplied complete conversation remains authoritative.
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

        // Keep backend execution sessions isolated from other Chat sessions.
        forge_request.user = turn.completion_id;

        // Route to streaming or non-streaming handler
        if (stream) {
            handleStreamingRequest(
                turn, forge_request, body, std::move(callback));
        } else {
            handleNonStreamingRequest(
                turn, forge_request, body, std::move(callback));
        }

    } catch (const json::exception& e) {
        if (active_turn.has_value()) {
            store_->abortTurn(
                active_turn->completion_id, active_turn->turn_id);
        }
        LOG_ERROR << "JSON parse error in createChatCompletion: " << e.what();
        callback(formatErrorResponse(
            std::string("Invalid JSON: ") + e.what(),
            "invalid_request_error",
            k400BadRequest
        ));
    } catch (const std::exception& e) {
        if (active_turn.has_value()) {
            store_->abortTurn(
                active_turn->completion_id, active_turn->turn_id);
        }
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
    const BeginChatTurnResult& turn,
    const CreateChatCompletionRequest& request,
    const json& request_body,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    auto stream_resp = HttpResponse::newAsyncStreamResponse(
        [turn, request, request_body, this](drogon::ResponseStreamPtr stream_ptr) {
            // Convert to shared_ptr for lambda capture
            auto stream = std::shared_ptr<drogon::ResponseStream>(std::move(stream_ptr));

            // Setup qai-forge callbacks
            StreamCallbacks callbacks;

            // Accumulated content for session update
            auto accumulated_content = std::make_shared<std::string>();
            auto accumulated_tool_calls = std::make_shared<json>();
            auto has_tool_calls = std::make_shared<bool>(false);

            callbacks.onToken = [stream, turn, accumulated_content,
                                 accumulated_tool_calls,
                                 has_tool_calls](const StreamChunk& chunk) {
                // Accumulate content
                if (chunk.content_delta) {
                    *accumulated_content += *chunk.content_delta;
                }

                // Build delta object
                json delta_obj = json::object();
                if (chunk.role.has_value()) {
                    delta_obj["role"] = *chunk.role;
                }
                if (chunk.content_delta.has_value()) {
                    delta_obj["content"] = *chunk.content_delta;
                }
                if (chunk.tool_calls.has_value() && chunk.tool_calls->is_array()
                        && !chunk.tool_calls->empty()) {
                    delta_obj["tool_calls"] = *chunk.tool_calls;
                    *has_tool_calls = true;
                }

                // Skip empty deltas (nothing to send to client)
                if (delta_obj.empty()) {
                    return;
                }

                // Format as OpenAI chat completion chunk
                json chunk_json = {
                    {"id", turn.completion_id},
                    {"object", "chat.completion.chunk"},
                    {"created", std::time(nullptr)},
                    {"model", turn.model},
                    {"choices", json::array({
                        {
                            {"index", 0},
                            {"delta", delta_obj},
                            {"finish_reason", nullptr}
                        }
                    })}
                };

                // Send as SSE
                std::string sse = "data: " + chunk_json.dump() + "\n\n";
                stream->send(sse);
            };

            callbacks.onComplete = [stream, turn, request_body, accumulated_content,
                                    accumulated_tool_calls, has_tool_calls,
                                    this](
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
                }

                json replay_result = {
                    {"content", *accumulated_content},
                    {"finish_reason", *has_tool_calls ? "tool_calls" : "stop"},
                    {"prompt_tokens", final_response.prompt_tokens},
                    {"completion_tokens", final_response.completion_tokens},
                    {"total_tokens", final_response.total_tokens}
                };
                if (*has_tool_calls && final_response.tool_calls.has_value()) {
                    replay_result["tool_calls"] = *final_response.tool_calls;
                }

                if (!store_->completeTurn(
                        turn.completion_id, turn.turn_id, assistant_msg,
                        request_body, replay_result)) {
                    LOG_WARN << "Discarding late streaming completion for session "
                             << turn.completion_id;
                    stream->close();
                    return;
                }

                // Send final chunk with finish_reason
                json final_delta = json::object();
                if (*has_tool_calls && final_response.tool_calls.has_value()) {
                    json tool_calls_delta = json::array();
                    int tool_index = 0;
                    for (const auto& tool_call : *final_response.tool_calls) {
                        const json function = tool_call.value(
                            "function", json::object());
                        tool_calls_delta.push_back({
                            {"index", tool_index++},
                            {"id", tool_call.value("id", "")},
                            {"type", tool_call.value("type", "function")},
                            {"function", {
                                {"name", function.value("name", "")},
                                {"arguments", function.value("arguments", "")}
                            }}
                        });
                    }
                    final_delta["tool_calls"] = std::move(tool_calls_delta);
                }

                json final_chunk = {
                    {"id", turn.completion_id},
                    {"object", "chat.completion.chunk"},
                    {"created", std::time(nullptr)},
                    {"model", turn.model},
                    {"choices", json::array({
                        {
                            {"index", 0},
                            {"delta", final_delta},
                            {"finish_reason", *has_tool_calls ? "tool_calls" : "stop"}
                        }
                    })}
                };

                std::string final_sse = "data: " + final_chunk.dump() + "\n\n";
                stream->send(final_sse);

                std::string done_sse = "data: [DONE]\n\n";
                stream->send(done_sse);
                stream->close();

                LOG_INFO << "Streaming completed for session "
                         << turn.completion_id;
            };

            callbacks.onError = [stream, turn, this](const GenAIException& error) {
                store_->abortTurn(turn.completion_id, turn.turn_id);
                LOG_ERROR << "Streaming error for session " << turn.completion_id
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

            callbacks.onCancelled = [stream, turn, this]() {
                store_->abortTurn(turn.completion_id, turn.turn_id);
                LOG_INFO << "Streaming cancelled for session "
                         << turn.completion_id;
                stream->close();
            };

            // Start generation
            try {
                GenerateOptions options =
                    makeChatGenerateOptions(turn, request.messages);

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
    const BeginChatTurnResult& turn,
    const CreateChatCompletionRequest& request,
    const json& request_body,
    std::function<void(const HttpResponsePtr&)>&& callback
) {
    try {
        // Prepare options
        GenerateOptions options =
            makeChatGenerateOptions(turn, request.messages);

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
        }

        json replay_result = {
            {"content", getStringOrDefault(assistant_msg, "content")},
            {"finish_reason", has_tool_calls ? "tool_calls" : "stop"},
            {"prompt_tokens", response.prompt_tokens},
            {"completion_tokens", response.completion_tokens},
            {"total_tokens", response.total_tokens}
        };
        if (has_tool_calls) {
            replay_result["tool_calls"] = *response.tool_calls;
        }

        if (!store_->completeTurn(
                turn.completion_id, turn.turn_id, assistant_msg,
                request_body, replay_result)) {
            callback(formatErrorResponse(
                "Chat completion was cancelled or deleted",
                "invalid_request_error",
                k409Conflict));
            return;
        }

        // Build usage object separately to avoid nested initializer issues
        json usage_obj = {
            {"prompt_tokens", response.prompt_tokens},
            {"completion_tokens", response.completion_tokens},
            {"total_tokens", response.total_tokens}
        };

        // Format OpenAI-compatible response
        json response_json = {
            {"id", turn.completion_id},
            {"object", "chat.completion"},
            {"created", std::time(nullptr)},
            {"model", turn.model},
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

        LOG_INFO << "Non-streaming completed for session "
                 << turn.completion_id;

    } catch (const GenAIException& e) {
        store_->abortTurn(turn.completion_id, turn.turn_id);
        LOG_ERROR << "Generation error for session " << turn.completion_id
                  << ": " << e.message;

        callback(formatErrorResponse(
            e.message,
            "server_error",
            static_cast<HttpStatusCode>(e.http_status)
        ));

    } catch (const std::exception& e) {
        store_->abortTurn(turn.completion_id, turn.turn_id);
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
        std::optional<ChatSession> session = store_->getSession(completion_id);
        if (!session.has_value()) {
            callback(formatErrorResponse(
                "Session not found",
                "not_found_error",
                k404NotFound
            ));
            return;
        }

        // Cancel active job if any
        if (!session->active_turn_id.empty()) {
            qai_forge::QaiForge::getInstance().cancel(completion_id);
        }

        // Delete session
        store_->deleteSession(completion_id);
        QaiForge::getInstance().releaseConversation(
            kChatMemoryNamespace, completion_id);

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
        std::optional<ChatSession> session = store_->getSession(completion_id);
        if (!session.has_value()) {
            callback(formatErrorResponse(
                "Session not found",
                "not_found_error",
                k404NotFound
            ));
            return;
        }

        // Cancel active job
        bool cancelled = false;
        if (!session->active_turn_id.empty()) {
            cancelled = qai_forge::QaiForge::getInstance().cancel(completion_id);
            if (cancelled) {
                store_->abortTurn(
                    completion_id, session->active_turn_id);
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
