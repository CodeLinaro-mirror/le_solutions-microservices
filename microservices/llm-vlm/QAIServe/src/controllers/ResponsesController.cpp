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
#include "mcp/McpAgenticLoop.h"
#include "mcp/McpClientRegistry.h"
#include "mcp/NativeToolRegistry.h"
#include "qai_forge/orchestration/ChatOrchestrator.h"
#include "qai_forge/InternalDTOs.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <random>
#include <iostream>

using json = nlohmann::ordered_json;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
static std::string generate_response_id() {
    static std::mt19937_64 rng(std::random_device{}());
    std::ostringstream oss;
    oss << "resp_" << std::hex << std::setw(16) << std::setfill('0') << rng();
    return oss.str();
}

static HttpResponsePtr make_error_response(int status_code, const std::string& message,
                                            const std::string& error_type = "server_error") {
    json error_body = {
        {"error", {
            {"message", message},
            {"type", error_type},
            {"param", nullptr},
            {"code", status_code}
        }}
    };
    auto resp = HttpResponse::newHttpJsonResponse(error_body.dump());
    resp->setStatusCode(static_cast<HttpStatusCode>(status_code));
    return resp;
}

static int current_unix_time() {
    return static_cast<int>(
        std::chrono::system_clock::now().time_since_epoch().count() / 1000000000LL);
}

// ─────────────────────────────────────────────────────────────────────────────
// Convert Responses API `input` to ChatCompletionRequest `messages[]`
//
// Responses API input formats:
//   1. String: "Hello" → [{role: "user", content: "Hello"}]
//   2. Array of message objects: [{role: "user", content: "..."}]
//   3. Array of content parts: [{type: "text", text: "..."}]
//   4. Array with function_call_output items (tool results from previous turn)
// ─────────────────────────────────────────────────────────────────────────────
static json input_to_messages(const json& input, const std::string& system_prompt = "") {
    json messages = json::array();

    if (!system_prompt.empty()) {
        messages.push_back({{"role", "system"}, {"content", system_prompt}});
    }

    if (input.is_string()) {
        messages.push_back({{"role", "user"}, {"content", input.get<std::string>()}});
    } else if (input.is_array()) {
        for (const auto& item : input) {
            if (item.is_object()) {
                std::string item_type = item.value("type", "");

                // Handle function_call_output items (tool results from previous turn)
                if (item_type == "function_call_output") {
                    messages.push_back({
                        {"role",         "tool"},
                        {"tool_call_id", item.value("call_id", "")},
                        {"content",      item.value("output", "")}
                    });
                    continue;
                }

                // Handle function_call items (assistant tool call from previous turn)
                if (item_type == "function_call") {
                    json tool_call = {
                        {"id",   item.value("call_id", item.value("id", ""))},
                        {"type", "function"},
                        {"function", {
                            {"name",      item.value("name", "")},
                            {"arguments", item.value("arguments", "{}")}
                        }}
                    };
                    messages.push_back({
                        {"role",       "assistant"},
                        {"content",    nullptr},
                        {"tool_calls", json::array({tool_call})}
                    });
                    continue;
                }

                // Standard message object
                std::string role = item.value("role", "user");
                if (item.contains("content")) {
                    messages.push_back({{"role", role}, {"content", item["content"]}});
                } else if (item.contains("text")) {
                    messages.push_back({{"role", role}, {"content", item["text"]}});
                }
            } else if (item.is_string()) {
                messages.push_back({{"role", "user"}, {"content", item.get<std::string>()}});
            }
        }
    }

    return messages;
}

// ─────────────────────────────────────────────────────────────────────────────
// Build Responses API output[] from a StandardResponse DTO
//
// Per the OpenAI Responses API spec, the reasoning output item is a SEPARATE
// top-level item in output[], NOT nested inside message.content[].
// ─────────────────────────────────────────────────────────────────────────────
static json build_output_array(const StandardResponse& result,
                                 const std::vector<McpCallRecord>& mcp_records = {}) {
    json output = json::array();

    // MCP call records come first (they happened before the final answer)
    for (const auto& record : mcp_records) {
        output.push_back(record.to_output_item());
    }

    // ── Reasoning output item (separate top-level item, NOT inside message) ──
    if (result.reasoning_content.has_value() && !result.reasoning_content.value().empty()) {
        output.push_back({
            {"type", "reasoning"},
            {"id",   "rs_" + result.id},
            {"summary", json::array({{
                {"type", "summary_text"},
                {"text", result.reasoning_content.value()}
            }})}
        });
    }

    // ── Message output item (answer text only) ────────────────────────────────
    json content_array = json::array();
    if (result.content.has_value() && !result.content.value().empty()) {
        content_array.push_back({
            {"type", "output_text"},
            {"text", result.content.value()}
        });
    }

    output.push_back({
        {"type",    "message"},
        {"id",      "msg_" + result.id},
        {"role",    "assistant"},
        {"content", content_array},
        {"status",  "completed"}
    });

    // Function call output items (non-MCP tool calls, if any)
    if (result.tool_calls.has_value() && !result.tool_calls.value().empty()) {
        for (const auto& tc : result.tool_calls.value()) {
            output.push_back({
                {"type",      "function_call"},
                {"id",        tc.value("id", "")},
                {"call_id",   tc.value("id", "")},
                {"name",      tc.value("function", json::object()).value("name", "")},
                {"arguments", tc.value("function", json::object()).value("arguments", "")}
            });
        }
    }

    return output;
}

// ─────────────────────────────────────────────────────────────────────────────
// Build a complete Responses API response object
// ─────────────────────────────────────────────────────────────────────────────
static json build_response_object(const std::string& response_id,
                                   const std::string& model,
                                   const json& output,
                                   const std::string& status,
                                   int prompt_tokens, int completion_tokens,
                                   bool truncated = false) {
    json obj = {
        {"id",               response_id},
        {"object",           "response"},
        {"created_at",       current_unix_time()},
        {"model",            model},
        {"status",           status},
        {"output",           output},
        {"usage", {
            {"input_tokens",  prompt_tokens},
            {"output_tokens", completion_tokens},
            {"total_tokens",  prompt_tokens + completion_tokens}
        }},
        {"error",            nullptr},
        {"incomplete_details", truncated
            ? json({{"reason", "max_tool_calls"}})
            : json(nullptr)}
    };
    return obj;
}

// ─────────────────────────────────────────────────────────────────────────────
// Parse MCP tool entries from the tools[] array
//
// Returns a struct describing what MCP servers are requested and their
// allowed_tools filters.
// ─────────────────────────────────────────────────────────────────────────────
struct McpToolRequest {
    std::string              server_label;
    std::string              server_url;    // optional: dynamic server URL
    std::vector<std::string> allowed_tools; // empty = all tools allowed
};

static std::vector<McpToolRequest> extract_mcp_tool_requests(const json& tools) {
    std::vector<McpToolRequest> requests;
    for (const auto& tool : tools) {
        if (!tool.is_object()) continue;
        if (tool.value("type", "") != "mcp") continue;

        McpToolRequest req;
        req.server_label = tool.value("server_label", "");
        req.server_url   = tool.value("server_url", "");

        if (tool.contains("allowed_tools") && tool["allowed_tools"].is_array()) {
            for (const auto& t : tool["allowed_tools"]) {
                req.allowed_tools.push_back(t.get<std::string>());
            }
        }
        requests.push_back(req);
    }
    return requests;
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
            for (const auto& r : extract_mcp_tool_requests(body["tools"])) {
                bool already_present = false;
                for (const auto& existing : mcp_requests) {
                    if (existing.server_label == r.server_label) {
                        already_present = true; break;
                    }
                }
                if (!already_present) mcp_requests.push_back(r);
            }
        }
    }

    // ── Auto-inject native tools ──────────────────────────────────────────────
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
                mcp_requests.push_back(native_req);
            }
            has_mcp_tools = true;
        }
    }

    // Convert input to messages format
    json messages = input_to_messages(body["input"], system_prompt);
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

    std::string response_id = generate_response_id();

    // ── MCP path ──────────────────────────────────────────────────────────────
    if (has_mcp_tools) {
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

                        int created_time = current_unix_time();

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
                            json output = build_output_array(
                                loop_result.final_response, loop_result.call_records);

                            // response.completed
                            emit_event("response.completed", {
                                {"type", "response.completed"},
                                {"response", build_response_object(
                                    response_id, model, output, "completed",
                                    loop_result.final_response.prompt_tokens,
                                    loop_result.final_response.completion_tokens,
                                    loop_result.truncated)}
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
                McpLoopResult loop_result = loop.run(sdk_request, mcp_function_tools);

                json output = build_output_array(
                    loop_result.final_response, loop_result.call_records);
                json response_obj = build_response_object(
                    response_id, model, output,
                    loop_result.truncated ? "incomplete" : "completed",
                    loop_result.final_response.prompt_tokens,
                    loop_result.final_response.completion_tokens,
                    loop_result.truncated);

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

    auto& orchestrator = ChatOrchestrator::getInstance();

    try {
        if (streaming) {
            // ── Streaming: emit Responses API SSE events ──────────────────────
            auto resp = HttpResponse::newAsyncStreamResponse(
                [&orchestrator, sdk_request, response_id, model](ResponseStreamPtr stream) {
                    auto emit_event = [&stream](const std::string& event_type,
                                                 const json& data) {
                        stream->send("event: " + event_type + "\n");
                        stream->send("data: " + data.dump() + "\n\n");
                    };

                    int created_time = current_unix_time();

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
                        orchestrator.handleStreaming(streaming_req,
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
                            });
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
            StandardResponse result = orchestrator.handleBlocking(sdk_request);

            // Build output array.
            // build_output_array() automatically emits a separate top-level
            // {"type":"reasoning"} item when reasoning_content is non-empty,
            // per the OpenAI Responses API spec.
            json output = build_output_array(result);

            // Build response object
            json response_obj = build_response_object(
                response_id, model, output, "completed",
                result.prompt_tokens, result.completion_tokens);

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

    auto& orchestrator = ChatOrchestrator::getInstance();
    bool cancelled = orchestrator.cancelSession(response_id);

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
