// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// ResponsesController — Layer 1 (Drogon HTTP Adapter for Responses API)
//
// Implements the OpenAI Responses API (limited on-device subset).
// Calls ChatOrchestrator (qai-forge SDK) and formats results into the
// Responses API wire format.
//
// MCP support (Layer 1.5):
//   When tools[] contains {type:"mcp"} entries, the request is routed through
//   McpAgenticLoop which drives the autonomous tool-call cycle. The loop
//   translates MCP tool schemas to function format before passing to Layer 2.
//
// Input conversion:
//   Responses API `input` (string or array) → ChatCompletionRequest `messages[]`
//   `previous_response_id` → session lookup via SessionManager
//
// Output conversion:
//   StandardResponse / StreamChunk → Responses API `output[]` array
//   McpCallRecord[]               → `mcp_call` output items
// ─────────────────────────────────────────────────────────────────────────────

#include "controllers/ResponsesController.h"
#include "ResponsesUtils.h"
#include "mcp/McpAgenticLoop.h"
#include "mcp/McpClientRegistry.h"
#include "mcp/NativeToolRegistry.h"
#include "qai_forge/orchestration/ChatOrchestrator.h"
#include "qai_forge/InternalDTOs.h"
#include "qai_forge/utils/Logger.h"
#include "scheduler/ModelScheduler.h"
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <iostream>

using json = nlohmann::ordered_json;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
static HttpResponsePtr make_error_response(int status_code, const std::string& message,
                                            const std::string& error_type = "server_error",
                                            const std::string& param = "") {
    json error_body = {
        {"error", {
            {"message", message},
            {"type", error_type},
            {"param", param.empty() ? json(nullptr) : json(param)},
            {"code", status_code}
        }}
    };
    auto resp = HttpResponse::newHttpJsonResponse(error_body.dump());
    resp->setStatusCode(static_cast<HttpStatusCode>(status_code));
    return resp;
}

// ─────────────────────────────────────────────────────────────────────────────
// POST /v1/responses — Create a response
// ─────────────────────────────────────────────────────────────────────────────
void ResponsesController::createResponse(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {

    // Parse request body
    json body;
    try {
        body = json::parse(req->getBody());
    } catch (...) {
        callback(make_error_response(400, "Invalid JSON body", "invalid_request_error"));
        return;
    }

    // Validate required fields
    if (!body.contains("model") || !body.contains("input")) {
        callback(make_error_response(400, "Missing required fields: 'model' and 'input'",
                                     "invalid_request_error"));
        return;
    }

    std::string model              = body.value("model", "");
    bool        streaming          = body.value("stream", false);
    std::string system_prompt      = body.value("instructions", "");
    std::string previous_response_id = body.value("previous_response_id", "");

    // ── Parse reasoning parameters ────────────────────────────────────────────
    std::string reasoning_effort  = "medium";  // default
    std::string reasoning_summary = "";         // "" = no summary output item
    if (body.contains("reasoning") && body["reasoning"].is_object()) {
        reasoning_effort  = body["reasoning"].value("effort",  "medium");
        reasoning_summary = body["reasoning"].value("summary", "");
    }

    // ── Tool type analysis ────────────────────────────────────────────────────
    bool has_mcp_tools      = false;
    bool has_function_tools = false;

    std::vector<McpToolRequest> mcp_requests;
    json function_tools = json::array();

    if (body.contains("tools") && body["tools"].is_array()) {
        for (const auto& tool : body["tools"]) {
            std::string tool_type = tool.value("type", "");

            if (tool_type == "mcp") {
                has_mcp_tools = true;
                // Parsed below via extract_mcp_tool_requests()
            } else if (tool_type == "function") {
                has_function_tools = true;
                function_tools.push_back(tool);
            } else if (tool_type == "web_search") {
                // Map OpenAI built-in web_search → native tool (if registered).
                // No HTTP 400 — if a web_search native tool is registered it
                // will be included; if not, the model simply won't see it.
                has_mcp_tools = true;
                McpToolRequest web_req;
                web_req.server_label  = "native";
                web_req.allowed_tools = {"web_search"};
                mcp_requests.push_back(web_req);
            } else if (tool_type == "file_search" || tool_type == "code_interpreter") {
                callback(make_error_response(400,
                    "Built-in tool '" + tool_type + "' is not available on-device. "
                    "Use 'function' or 'mcp' tool types.",
                    "invalid_request_error"));
                return;
            }
        }

        if (has_mcp_tools) {
            // Merge any {type:"mcp"} entries from the body into mcp_requests,
            // avoiding duplicates with entries already added (e.g. web_search).
            for (const auto& r : ResponsesUtils::extract_mcp_tool_requests(body["tools"])) {
                bool already_present = false;
                for (const auto& existing : mcp_requests) {
                    if (existing.server_label == r.server_label) {
                        already_present = true;
                        break;
                    }
                }
                if (!already_present) mcp_requests.push_back(r);
            }
        }
    }

    // ── Auto-inject native tools ──────────────────────────────────────────────
    // Always include built-in native tools (datetime, calculator, etc.) so the
    // model knows what capabilities are available on every request.
    // Their schemas are injected into the model's tool context automatically.
    {
        auto& native_reg = NativeToolRegistry::getInstance();
        auto& mcp_reg    = McpClientRegistry::getInstance();

        if (native_reg.hasTools() && mcp_reg.hasServer("native")) {
            bool has_native = false;
            for (const auto& r : mcp_requests) {
                if (r.server_label == "native") { has_native = true; break; }
            }
            if (!has_native) {
                McpToolRequest native_req;
                native_req.server_label = "native";
                // allowed_tools empty = all native tools included
                mcp_requests.push_back(native_req);
            }
            has_mcp_tools = true;
        }
    }

    // Convert input to messages format
    json messages = ResponsesUtils::input_to_messages(body["input"], system_prompt);
    if (messages.empty()) {
        callback(make_error_response(400, "Input produced no messages", "invalid_request_error"));
        return;
    }

    // Build CreateChatCompletionRequest for the SDK
    CreateChatCompletionRequest sdk_request;
    try {
        json sdk_body = {
            {"model",    model},
            {"messages", messages},
            {"stream",   false}   // always blocking at SDK level; we handle streaming above
        };
        if (body.contains("max_output_tokens") && !body["max_output_tokens"].is_null())
            sdk_body["max_completion_tokens"] = body["max_output_tokens"];
        if (body.contains("temperature") && !body["temperature"].is_null())
            sdk_body["temperature"] = body["temperature"];
        if (body.contains("top_p") && !body["top_p"].is_null())
            sdk_body["top_p"] = body["top_p"];
        if (!previous_response_id.empty())
            sdk_body["user"] = previous_response_id;
        // Pass reasoning parameters to the SDK (used by ReasoningBudgetCalculator)
        sdk_body["reasoning_effort"]  = reasoning_effort;
        if (!reasoning_summary.empty())
            sdk_body["reasoning_summary"] = reasoning_summary;

        sdk_request = CreateChatCompletionRequest::from_json(sdk_body);
    } catch (const std::exception& e) {
        callback(make_error_response(400,
            std::string("Request parsing error: ") + e.what(), "invalid_request_error"));
        return;
    }

    std::string response_id = ResponsesUtils::generate_response_id();
    LOG_INFO("[ResponsesController] Create response: response=" << response_id
             << " model=" << model
             << " stream=" << (streaming ? "true" : "false")
             << " previous=" << previous_response_id
             << " has_mcp=" << (has_mcp_tools ? "true" : "false")
             << " has_function_tools=" << (has_function_tools ? "true" : "false"));

    // ── MCP path ──────────────────────────────────────────────────────────────
    if (has_mcp_tools) {
        LOG_INFO("[ResponsesController] Routing through MCP loop: response="
                 << response_id << " model=" << model
                 << " mcp_server_count=" << mcp_requests.size()
                 << " stream=" << (streaming ? "true" : "false"));
        auto& registry = McpClientRegistry::getInstance();

        // Validate that all requested MCP servers are registered
        for (const auto& mcp_req : mcp_requests) {
            if (!mcp_req.server_label.empty() && !registry.hasServer(mcp_req.server_label)) {
                callback(make_error_response(400,
                    "MCP server '" + mcp_req.server_label + "' is not registered. "
                    "Check mcp_servers.json configuration.",
                    "invalid_request_error"));
                return;
            }
        }

        // Aggregate MCP tools (translated to function format)
        json mcp_function_tools = json::array();
        for (const auto& mcp_req : mcp_requests) {
            json server_tools = registry.getAllTools(mcp_req.server_label,
                                                      mcp_req.allowed_tools);
            for (const auto& t : server_tools) {
                mcp_function_tools.push_back(t);
            }
        }

        // Also include any explicit function tools from the request
        for (const auto& ft : function_tools) {
            mcp_function_tools.push_back(ft);
        }

        // Read max_iterations from env
        const char* max_iter_env = std::getenv("RESPONSES_MCP_MAX_ITERATIONS");
        int max_iterations = max_iter_env ? std::stoi(max_iter_env) : 10;

        try {
            if (streaming) {
                // ── MCP Streaming ─────────────────────────────────────────────
                auto resp = HttpResponse::newAsyncStreamResponse(
                    [sdk_request, mcp_function_tools, response_id, model,
                     max_iterations](ResponseStreamPtr stream) {

                        auto emit_event = [&stream](const std::string& event_type,
                                                     const json& data) {
                            stream->send("event: " + event_type + "\n");
                            stream->send("data: " + data.dump() + "\n\n");
                        };

                        int created_time = ResponsesUtils::current_unix_time();

                        // response.created
                        emit_event("response.created", {
                            {"type", "response.created"},
                            {"response", {
                                {"id",         response_id},
                                {"object",     "response"},
                                {"created_at", created_time},
                                {"model",      model},
                                {"status",     "in_progress"},
                                {"output",     json::array()}
                            }}
                        });

                        // response.output_item.added (placeholder for first item)
                        emit_event("response.output_item.added", {
                            {"type",         "response.output_item.added"},
                            {"output_index", 0},
                            {"item", {
                                {"type",   "message"},
                                {"id",     "msg_" + response_id},
                                {"role",   "assistant"},
                                {"content", json::array()},
                                {"status", "in_progress"}
                            }}
                        });

                        McpLoopResult loop_result;
                        bool had_error = false;
                        std::string error_msg;

                        try {
                            auto& registry = McpClientRegistry::getInstance();
                            McpAgenticLoop loop(registry, max_iterations);

                            McpSseEmitter emitter = [&emit_event](
                                const std::string& event_type, const json& data) {
                                emit_event(event_type, data);
                            };

                            loop_result = loop.runStreaming(
                                sdk_request, mcp_function_tools, emitter, response_id);

                        } catch (const GenAIException& e) {
                            had_error = true;
                            error_msg = e.message;
                        } catch (const std::exception& e) {
                            had_error = true;
                            error_msg = e.what();
                        }

                        if (!had_error) {
                            std::string final_text =
                                loop_result.final_response.content.value_or("");
                            int output_index =
                                static_cast<int>(loop_result.call_records.size());

                            // response.output_item.done
                            emit_event("response.output_item.done", {
                                {"type",         "response.output_item.done"},
                                {"output_index", output_index},
                                {"item", {
                                    {"type",    "message"},
                                    {"id",      "msg_" + response_id},
                                    {"role",    "assistant"},
                                    {"content", {{{"type", "output_text"},
                                                  {"text", final_text}}}},
                                    {"status",  "completed"}
                                }}
                            });

                            // Build final output array
                            json output = ResponsesUtils::build_output_array(
                                loop_result.final_response, loop_result.call_records);

                            // response.completed
                            emit_event("response.completed", {
                                {"type", "response.completed"},
                                {"response", ResponsesUtils::build_response_object(
                                    response_id, model, output, "completed",
                                    loop_result.final_response.prompt_tokens,
                                    loop_result.final_response.completion_tokens,
                                    created_time,
                                    json(nullptr),
                                    loop_result.truncated
                                        ? json({{"reason", "max_tool_calls"}})
                                        : json(nullptr))}
                            });
                        } else {
                            emit_event("error", {
                                {"type",    "error"},
                                {"code",    "server_error"},
                                {"message", error_msg}
                            });
                        }

                        stream->send("data: [DONE]\n\n");
                        stream->close();
                    }
                );
                resp->addHeader("Content-Type", "text/event-stream");
                resp->addHeader("Cache-Control", "no-cache");
                resp->addHeader("Connection", "keep-alive");
                callback(resp);

            } else {
                // ── MCP Non-streaming ─────────────────────────────────────────
                McpAgenticLoop loop(registry, max_iterations);
                McpLoopResult loop_result =
                    loop.run(sdk_request, mcp_function_tools, response_id);

                json output = ResponsesUtils::build_output_array(
                    loop_result.final_response, loop_result.call_records);
                int created_time = ResponsesUtils::current_unix_time();
                json response_obj = ResponsesUtils::build_response_object(
                    response_id, model, output,
                    loop_result.truncated ? "incomplete" : "completed",
                    loop_result.final_response.prompt_tokens,
                    loop_result.final_response.completion_tokens,
                    created_time,
                    json(nullptr),
                    loop_result.truncated
                        ? json({{"reason", "max_tool_calls"}})
                        : json(nullptr));

                auto resp = HttpResponse::newHttpJsonResponse(response_obj.dump());
                resp->setStatusCode(k200OK);
                callback(resp);
            }

        } catch (const GenAIException& e) {
            std::string error_type = (e.http_status >= 500) ? "server_error"
                                                             : "invalid_request_error";
            callback(make_error_response(e.http_status, e.message, error_type));
        } catch (const std::exception& e) {
            callback(make_error_response(500,
                std::string("Internal server error: ") + e.what()));
        }
        return;
    }

    // ── Standard (non-MCP) path ───────────────────────────────────────────────
    // Pass function tools to the SDK if present
    if (has_function_tools) {
        sdk_request.tools = function_tools;
    }
    LOG_INFO("[ResponsesController] Routing through standard path: response="
             << response_id << " model=" << model
             << " stream=" << (streaming ? "true" : "false"));

    try {
        if (streaming) {
            // ── Streaming: emit Responses API SSE events ──────────────────────
            auto resp = HttpResponse::newAsyncStreamResponse(
                [sdk_request, response_id, previous_response_id, model](ResponseStreamPtr stream) {
                    auto emit_event = [&stream](const std::string& event_type,
                                                 const json& data) {
                        stream->send("event: " + event_type + "\n");
                        stream->send("data: " + data.dump() + "\n\n");
                    };

                    int created_time = ResponsesUtils::current_unix_time();

                    // response.created event
                    emit_event("response.created", {
                        {"type", "response.created"},
                        {"response", {
                            {"id",         response_id},
                            {"object",     "response"},
                            {"created_at", created_time},
                            {"model",      model},
                            {"status",     "in_progress"},
                            {"output",     json::array()}
                        }}
                    });

                    // response.output_item.added
                    emit_event("response.output_item.added", {
                        {"type",         "response.output_item.added"},
                        {"output_index", 0},
                        {"item", {
                            {"type",    "message"},
                            {"id",      "msg_" + response_id},
                            {"role",    "assistant"},
                            {"content", json::array()},
                            {"status",  "in_progress"}
                        }}
                    });

                    // response.content_part.added
                    emit_event("response.content_part.added", {
                        {"type",          "response.content_part.added"},
                        {"output_index",  0},
                        {"content_index", 0},
                        {"part",          {{"type", "output_text"}, {"text", ""}}}
                    });

                    std::string full_text;
                    bool had_error = false;
                    std::string error_msg;
                    int token_count = 0;

                    // Re-enable streaming for the SDK call
                    CreateChatCompletionRequest streaming_req = sdk_request;
                    streaming_req.stream = true;

                    try {
                        scheduler::SchedulerInvokeOptions invoke_options;
                        invoke_options.response_id = response_id;
                        invoke_options.previous_response_id = previous_response_id;
                        invoke_options.kind = scheduler::JobKind::HTTP_STREAMING;

                        scheduler::ModelScheduler::getInstance().runStreaming(
                            streaming_req,
                            [&](const StreamChunk& chunk) {
                                if (chunk.content_delta.has_value()
                                    && !chunk.content_delta.value().empty()) {
                                    full_text += chunk.content_delta.value();
                                    token_count++;
                                    emit_event("response.output_text.delta", {
                                        {"type",          "response.output_text.delta"},
                                        {"output_index",  0},
                                        {"content_index", 0},
                                        {"delta",         chunk.content_delta.value()}
                                    });
                                }
                                if (chunk.reasoning_content.has_value()
                                    && !chunk.reasoning_content.value().empty()) {
                                    emit_event("response.reasoning.delta", {
                                        {"type",         "response.reasoning.delta"},
                                        {"output_index", 0},
                                        {"delta",        chunk.reasoning_content.value()}
                                    });
                                }
                            },
                            invoke_options);
                    } catch (const GenAIException& e) {
                        had_error = true;
                        error_msg = e.message;
                    } catch (const std::exception& e) {
                        had_error = true;
                        error_msg = e.what();
                    }

                    if (!had_error) {
                        emit_event("response.output_text.done", {
                            {"type",          "response.output_text.done"},
                            {"output_index",  0},
                            {"content_index", 0},
                            {"text",          full_text}
                        });

                        emit_event("response.output_item.done", {
                            {"type",         "response.output_item.done"},
                            {"output_index", 0},
                            {"item", {
                                {"type",    "message"},
                                {"id",      "msg_" + response_id},
                                {"role",    "assistant"},
                                {"content", {{{"type", "output_text"},
                                              {"text", full_text}}}},
                                {"status",  "completed"}
                            }}
                        });

                        emit_event("response.completed", {
                            {"type", "response.completed"},
                            {"response", {
                                {"id",         response_id},
                                {"object",     "response"},
                                {"model",      model},
                                {"status",     "completed"},
                                {"output", {{
                                    {"type",    "message"},
                                    {"id",      "msg_" + response_id},
                                    {"role",    "assistant"},
                                    {"content", {{{"type", "output_text"},
                                                  {"text", full_text}}}},
                                    {"status",  "completed"}
                                }}},
                                {"usage", {
                                    {"input_tokens",  0},
                                    {"output_tokens", token_count},
                                    {"total_tokens",  token_count}
                                }}
                            }}
                        });
                    } else {
                        emit_event("error", {
                            {"type",    "error"},
                            {"code",    "server_error"},
                            {"message", error_msg}
                        });
                    }

                    stream->send("data: [DONE]\n\n");
                    stream->close();
                }
            );
            resp->addHeader("Content-Type", "text/event-stream");
            resp->addHeader("Cache-Control", "no-cache");
            resp->addHeader("Connection", "keep-alive");
            callback(resp);

        } else {
            // ── Non-streaming ─────────────────────────────────────────────────
            scheduler::SchedulerInvokeOptions invoke_options;
            invoke_options.response_id = response_id;
            invoke_options.previous_response_id = previous_response_id;
            invoke_options.kind = scheduler::JobKind::HTTP_NON_STREAMING;

            StandardResponse result =
                scheduler::ModelScheduler::getInstance().runBlocking(
                    sdk_request, invoke_options);

            // Build output array.
            // build_output_array() automatically emits a separate top-level
            // {"type":"reasoning"} item when reasoning_content is non-empty,
            // per the OpenAI Responses API spec.
            json output = ResponsesUtils::build_output_array(result);

            // Build response object
            int created_time = ResponsesUtils::current_unix_time();
            json response_obj = ResponsesUtils::build_response_object(
                response_id, model, output, "completed",
                result.prompt_tokens, result.completion_tokens,
                created_time, json(nullptr), json(nullptr));

            // Add output_tokens_details.reasoning_tokens when thinking occurred
            if (result.reasoning_tokens > 0) {
                response_obj["usage"]["output_tokens_details"] = {
                    {"reasoning_tokens", result.reasoning_tokens}
                };
            }

            auto resp = HttpResponse::newHttpJsonResponse(response_obj.dump());
            resp->setStatusCode(k200OK);
            callback(resp);
        }

    } catch (const GenAIException& e) {
        std::string error_type = (e.http_status >= 500) ? "server_error"
                                                         : "invalid_request_error";
        callback(make_error_response(e.http_status, e.message, error_type));
    } catch (const std::exception& e) {
        callback(make_error_response(500,
            std::string("Internal server error: ") + e.what()));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// GET /v1/responses/{response_id}
// ─────────────────────────────────────────────────────────────────────────────
void ResponsesController::getResponse(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    const std::string& response_id) {

    // The session IS the response in our implementation.
    // We look up the session by response_id and return the last assistant message.
    json response_obj = {
        {"id",     response_id},
        {"object", "response"},
        {"status", "completed"},
        {"model",  "unknown"},
        {"output", json::array()},
        {"usage",  {{"input_tokens", 0}, {"output_tokens", 0}, {"total_tokens", 0}}}
    };

    auto resp = HttpResponse::newHttpJsonResponse(response_obj.dump());
    resp->setStatusCode(k200OK);
    callback(resp);
}

// ─────────────────────────────────────────────────────────────────────────────
// DELETE /v1/responses/{response_id}
// ─────────────────────────────────────────────────────────────────────────────
void ResponsesController::deleteResponse(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    const std::string& response_id) {

    auto& orchestrator = ChatOrchestrator::getInstance();
    LOG_INFO("[ResponsesController] Delete response: response=" << response_id);
    bool deleted = orchestrator.deleteSession(response_id);

    if (!deleted) {
        callback(make_error_response(404, "Response " + response_id + " not found",
                                     "invalid_request_error"));
        return;
    }

    json response = {
        {"id",      response_id},
        {"object",  "response.deleted"},
        {"deleted", true}
    };
    auto resp = HttpResponse::newHttpJsonResponse(response.dump());
    resp->setStatusCode(k200OK);
    callback(resp);
}

// ─────────────────────────────────────────────────────────────────────────────
// POST /v1/responses/{response_id}/cancel
// ─────────────────────────────────────────────────────────────────────────────
void ResponsesController::cancelResponse(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    const std::string& response_id) {

    LOG_INFO("[ResponsesController] Cancel response: response=" << response_id);
    bool cancelled =
        scheduler::ModelScheduler::getInstance().cancelResponse(response_id);

    if (!cancelled) {
        callback(make_error_response(404,
            "Response " + response_id + " not found or not active",
            "invalid_request_error"));
        return;
    }

    json response = {
        {"id",     response_id},
        {"object", "response"},
        {"status", "cancelled"}
    };
    auto resp = HttpResponse::newHttpJsonResponse(response.dump());
    resp->setStatusCode(k200OK);
    callback(resp);
}
