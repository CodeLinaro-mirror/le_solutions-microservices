// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// ResponsesController — Layer 1 (Drogon HTTP Adapter for Responses API)
//
// Implements the OpenAI Responses API (limited on-device subset).
// Calls the model scheduler and formats results into the
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
#include "ResponseStore.h"
#include "ResponsesConstants.h"
#include "ResponsesCompactionService.h"
#include "ResponsesUtils.h"
#include "TokenBudgetUtils.h"
#include "mcp/McpAgenticLoop.h"
#include "mcp/McpClientRegistry.h"
#include "mcp/NativeToolRegistry.h"
#include "qai_forge/InternalDTOs.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/Logger.h"
#include "scheduler/ModelScheduler.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <utility>

using json = nlohmann::ordered_json;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
enum class RetrieveStreamParam {
    Disabled,
    Enabled,
    Invalid
};

static RetrieveStreamParam parse_retrieve_stream_param(const std::string& raw) {
    if (raw.empty() || raw == "false" || raw == "False" || raw == "FALSE"
        || raw == "0") {
        return RetrieveStreamParam::Disabled;
    }
    if (raw == "true" || raw == "True" || raw == "TRUE" || raw == "1") {
        return RetrieveStreamParam::Enabled;
    }
    return RetrieveStreamParam::Invalid;
}

static bool parse_input_items_limit(const std::string& raw, int& limit) {
    if (raw.empty()) {
        limit = 20;
        return true;
    }
    if (!std::all_of(raw.begin(), raw.end(), [](unsigned char c) {
            return std::isdigit(c);
        })) {
        return false;
    }

    char* end = nullptr;
    long parsed = std::strtol(raw.c_str(), &end, 10);
    if (end != raw.c_str() + raw.size() || parsed < 1 || parsed > 100) {
        return false;
    }
    limit = static_cast<int>(parsed);
    return true;
}

static bool parse_input_items_order(const std::string& raw,
                                    std::string& order) {
    if (raw.empty()) {
        order = "desc";
        return true;
    }

    order = raw;
    std::transform(order.begin(), order.end(), order.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return order == "asc" || order == "desc";
}

static bool get_optional_string_field(const json& body,
                                      const std::string& field,
                                      std::string& value) {
    value.clear();
    if (!body.contains(field) || body[field].is_null()) {
        return true;
    }
    if (!body[field].is_string()) {
        return false;
    }
    value = body[field].get<std::string>();
    return true;
}

static json make_assistant_messages(const StandardResponse& result) {
    json message = {
        {"role", "assistant"},
        {"content", result.content.value_or("")}
    };
    if (result.reasoning_content.has_value()
        && !result.reasoning_content.value().empty()) {
        message["_thinking_content"] = result.reasoning_content.value();
    }
    if (result.tool_calls.has_value()
        && !result.tool_calls.value().empty()) {
        message["tool_calls"] = result.tool_calls.value();
    }
    return json::array({message});
}

static json make_store_error_object(const std::string& code,
                                    const std::string& message,
                                    const std::string& param = "") {
    json error = {
        {"code", code},
        {"message", message}
    };
    if (!param.empty()) {
        error["param"] = param;
    }
    return error;
}

static void add_reasoning_usage_details(json& response_obj,
                                        const StandardResponse& result) {
    if (result.reasoning_tokens <= 0) {
        return;
    }
    response_obj["usage"]["output_tokens_details"] = {
        {"reasoning_tokens", result.reasoning_tokens}
    };
}

static bool current_turn_has_tool_response(const json& messages) {
    if (!messages.is_array()) {
        return false;
    }
    for (const auto& message : messages) {
        if (message.is_object() && message.value("role", "") == "tool") {
            return true;
        }
    }
    return false;
}

static bool branch_ends_with_tool_call(const json& ancestor_messages) {
    if (!ancestor_messages.is_array() || ancestor_messages.empty()) {
        return false;
    }
    const auto& last = ancestor_messages.back();
    if (!last.is_object() || last.value("role", "") != "assistant") {
        return false;
    }
    return last.contains("tool_calls")
        && last["tool_calls"].is_array()
        && !last["tool_calls"].empty();
}

static void prepend_system_message(json& messages,
                                   const std::string& instructions) {
    if (instructions.empty()) {
        return;
    }
    json with_system = json::array();
    with_system.push_back({{"role", "system"}, {"content", instructions}});
    if (messages.is_array()) {
        for (const auto& message : messages) {
            with_system.push_back(message);
        }
    }
    messages = std::move(with_system);
}

static CreateChatCompletionRequest make_standard_sdk_request(
    const json& body,
    const std::string& model,
    const json& request_messages,
    const std::string& instructions,
    const std::string& reasoning_effort,
    const std::string& reasoning_summary,
    const json& function_tools,
    bool has_function_tools,
    std::optional<int> max_completion_tokens) {
    json sdk_messages = request_messages;
    prepend_system_message(sdk_messages, instructions);

    json sdk_body = {
        {"model", model},
        {"messages", sdk_messages},
        {"stream", false},
        {"reasoning_effort", reasoning_effort}
    };
    if (max_completion_tokens.has_value()) {
        sdk_body["max_completion_tokens"] = max_completion_tokens.value();
    }
    if (body.contains("temperature") && !body["temperature"].is_null()) {
        sdk_body["temperature"] = body["temperature"];
    }
    if (body.contains("top_p") && !body["top_p"].is_null()) {
        sdk_body["top_p"] = body["top_p"];
    }
    if (!reasoning_summary.empty()) {
        sdk_body["reasoning_summary"] = reasoning_summary;
    }
    if (has_function_tools) {
        sdk_body["tools"] = function_tools;
    }
    return CreateChatCompletionRequest::from_json(sdk_body);
}

static std::string generate_summary_with_scheduler(
    const std::string& model,
    const std::string& prompt,
    int max_output_tokens) {
    json sdk_body = {
        {"model", model},
        {"messages", json::array({{
            {"role", "user"},
            {"content", prompt}
        }})},
        {"stream", false},
        {"max_completion_tokens", max_output_tokens},
        {"temperature", 0.3f},
        {"reasoning_effort", "none"}
    };
    CreateChatCompletionRequest request =
        CreateChatCompletionRequest::from_json(sdk_body);

    scheduler::SchedulerInvokeOptions options;
    options.response_id = ResponsesUtils::generate_compaction_id();
    options.session_id = options.response_id;
    options.kind = scheduler::JobKind::INTERNAL_SUMMARIZATION;
    options.skip_summarization_middleware = true;
    options.use_response_history = true;
    options.response_history = json::array();

    StandardResponse result =
        scheduler::ModelScheduler::getInstance().runBlocking(request, options);
    return result.content.value_or("");
}

static HttpResponsePtr make_json_response(const json& body,
                                           HttpStatusCode status_code) {
    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(status_code);
    resp->addHeader("Content-Type", "application/json; charset=utf-8");
    resp->setBody(body.dump());
    return resp;
}

static HttpResponsePtr make_error_response(int status_code, const std::string& message,
                                            const std::string& error_type = "server_error",
                                            const std::string& param = "",
                                            const std::string& code = "") {
    json error_body = {
        {"error", {
            {"message", message},
            {"type", error_type},
            {"param", param.empty() ? json(nullptr) : json(param)},
            {"code", code.empty() ? json(nullptr) : json(code)}
        }}
    };
    return make_json_response(
        error_body,
        static_cast<HttpStatusCode>(status_code));
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

    json request_messages = ResponsesUtils::input_to_messages(body["input"], "");
    if (request_messages.empty()) {
        callback(make_error_response(400, "Input produced no messages", "invalid_request_error"));
        return;
    }

    // Convert input to messages format for the existing MCP path.
    json messages = ResponsesUtils::input_to_messages(body["input"], system_prompt);

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

    json metadata = json::object();
    if (body.contains("metadata") && !body["metadata"].is_null()) {
        if (!body["metadata"].is_object()) {
            callback(make_error_response(
                400,
                "invalid value for 'metadata' - expected object",
                "invalid_request_error",
                "metadata"));
            return;
        }
        metadata = body["metadata"];
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
                     max_iterations, previous_response_id,
                     metadata](ResponseStreamPtr stream) {

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
                                        : json(nullptr),
                                    previous_response_id,
                                    metadata)}
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
                        : json(nullptr),
                    previous_response_id,
                    metadata);

                auto resp = make_json_response(response_obj, k200OK);
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
    auto& config_mgr = ModelConfigManager::getInstance();
    if (model.empty() || !config_mgr.validateModel(model)) {
        callback(make_error_response(
            404,
            "Model '" + model + "' not found. Check /v1/models for available models.",
            "invalid_request_error",
            "model"));
        return;
    }

    std::string base_system_prompt = system_prompt.empty()
        ? std::string(ResponsesConstants::DEFAULT_SYSTEM_PROMPT)
        : system_prompt;
    std::string effective_system_prompt = base_system_prompt;
    json input_items =
        ResponsesUtils::normalize_input_items(response_id, body["input"]);
    json tools_for_budget = has_function_tools ? function_tools : json::array();

    ResponseStore& store = ResponseStore::getInstance();
    BuildCandidateResult walk =
        store.buildCandidateMessages(previous_response_id, request_messages);
    if (!walk.ok) {
        callback(make_error_response(
            walk.http_status,
            walk.error_message,
            "invalid_request_error",
            "previous_response_id"));
        return;
    }
    effective_system_prompt = ResponsesUtils::inject_summary_into_instructions(
        base_system_prompt,
        walk.applied_summary);

    const bool is_vlm = config_mgr.supportsVision(model);
    std::optional<int> requested_max_output_tokens =
        sdk_request.max_completion_tokens;
    std::optional<int> resolved_max_output_tokens =
        requested_max_output_tokens;

    if (!is_vlm) {
        bool skip_summarization =
            previous_response_id.empty()
            || current_turn_has_tool_response(walk.current_request_messages)
            || branch_ends_with_tool_call(walk.ancestor_messages);
        int context_size = config_mgr.getContextSize(model);
        int projected_max_output_tokens = requested_max_output_tokens.value_or(
            TokenBudgetUtils::default_max_output_tokens(context_size));
        TokenBudgetUtils::SummarizationTriggerResult trigger =
            TokenBudgetUtils::evaluate_summarization_trigger(
                model,
                walk.ancestor_messages,
                walk.current_request_messages,
                effective_system_prompt,
                tools_for_budget,
                projected_max_output_tokens);

        if (!skip_summarization && trigger.should_summarize) {
            ResponsesCompactionService::CompactBranchResult compact_result =
                ResponsesCompactionService::getInstance().compactBranch(
                    previous_response_id,
                    model,
                    [model](const std::string& prompt, int max_tokens) {
                        return generate_summary_with_scheduler(
                            model, prompt, max_tokens);
                    });
            if (!compact_result.ok) {
                TokenBudgetUtils::ContextBudgetResult fallback_budget =
                    TokenBudgetUtils::resolve_context_budget(
                        model,
                        walk.ancestor_messages,
                        walk.current_request_messages,
                        effective_system_prompt,
                        tools_for_budget,
                        requested_max_output_tokens);
                if (!fallback_budget.ok) {
                    callback(make_error_response(
                        500,
                        "summary generation failed and the request cannot proceed without it",
                        "server_error"));
                    return;
                }
                resolved_max_output_tokens =
                    fallback_budget.resolved_max_output_tokens;
                LOG_WARN("[ResponsesController] Summary generation failed but "
                         "request fits without compaction: response="
                         << response_id
                         << " error=\"" << compact_result.error_message << "\"");
            } else {
                walk = store.buildCandidateMessages(
                    previous_response_id,
                    request_messages);
                if (!walk.ok) {
                    callback(make_error_response(
                        walk.http_status,
                        walk.error_message,
                        "invalid_request_error",
                        "previous_response_id"));
                    return;
                }
                effective_system_prompt =
                    ResponsesUtils::inject_summary_into_instructions(
                        base_system_prompt,
                        walk.applied_summary);
            }
        }

        if (!resolved_max_output_tokens.has_value()
            || resolved_max_output_tokens == requested_max_output_tokens) {
            TokenBudgetUtils::ContextBudgetResult budget =
                TokenBudgetUtils::resolve_context_budget(
                    model,
                    walk.ancestor_messages,
                    walk.current_request_messages,
                    effective_system_prompt,
                    tools_for_budget,
                    requested_max_output_tokens);
            if (!budget.ok) {
                callback(make_error_response(
                    budget.http_status,
                    budget.error_message,
                    "invalid_request_error",
                    budget.error_param,
                    budget.error_code));
                return;
            }
            resolved_max_output_tokens =
                budget.resolved_max_output_tokens;
        }
    }

    BeginResponseResult begin = store.beginResponse(
        response_id,
        model,
        previous_response_id,
        input_items,
        walk.current_request_messages,
        metadata);
    if (!begin.ok) {
        callback(make_error_response(
            begin.http_status,
            begin.error_message,
            "invalid_request_error",
            "previous_response_id"));
        return;
    }

    CreateChatCompletionRequest standard_request;
    try {
        standard_request = make_standard_sdk_request(
            body,
            model,
            begin.current_request_messages,
            effective_system_prompt,
            reasoning_effort,
            reasoning_summary,
            function_tools,
            has_function_tools,
            is_vlm ? requested_max_output_tokens : resolved_max_output_tokens);
    } catch (const std::exception& e) {
        json error_obj = make_store_error_object(
            "invalid_request_error",
            std::string("Request parsing error: ") + e.what());
        store.failResponse(response_id, error_obj);
        callback(make_error_response(
            400,
            std::string("Request parsing error: ") + e.what(),
            "invalid_request_error"));
        return;
    }

    scheduler::SchedulerInvokeOptions invoke_options;
    invoke_options.response_id = response_id;
    invoke_options.session_id = response_id;
    invoke_options.kind = streaming
        ? scheduler::JobKind::HTTP_STREAMING
        : scheduler::JobKind::HTTP_NON_STREAMING;
    invoke_options.skip_summarization_middleware = true;
    invoke_options.use_response_history = true;
    invoke_options.response_history = begin.ancestor_messages;

    std::optional<StoredResponse> pre_submit = store.getResponse(response_id);
    if (!pre_submit.has_value()
        || pre_submit->status != StoredResponseStatus::InProgress) {
        json response_obj = pre_submit.has_value()
            ? pre_submit->response_object
            : json::object();
        auto resp = make_json_response(
            response_obj,
            pre_submit.has_value() ? k200OK : k404NotFound);
        callback(resp);
        return;
    }

    LOG_INFO("[ResponsesController] Routing through standard path: response="
             << response_id << " model=" << model
             << " stream=" << (streaming ? "true" : "false"));

    try {
        if (streaming) {
            // ── Streaming: emit Responses API SSE events ──────────────────────
            auto resp = HttpResponse::newAsyncStreamResponse(
                [standard_request, invoke_options, response_id, model,
                 previous_response_id, metadata,
                 begin_created_at = begin.created_at](ResponseStreamPtr stream) {
                    auto emit_event = [&stream](const std::string& event_type,
                                                 const json& data) {
                        stream->send("event: " + event_type + "\n");
                        stream->send("data: " + data.dump() + "\n\n");
                    };

                    // response.created event
                    emit_event("response.created", {
                        {"type", "response.created"},
                        {"response", {
                            {"id",         response_id},
                            {"object",     "response"},
                            {"created_at", begin_created_at},
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
                    int http_status = 500;
                    StandardResponse result;

                    // Re-enable streaming for the SDK call
                    CreateChatCompletionRequest streaming_req = standard_request;
                    streaming_req.stream = true;

                    try {
                        result = scheduler::ModelScheduler::getInstance().runStreaming(
                            streaming_req,
                            [&](const StreamChunk& chunk) {
                                if (chunk.content_delta.has_value()
                                    && !chunk.content_delta.value().empty()) {
                                    full_text += chunk.content_delta.value();
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
                        http_status = e.http_status;
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

                        json output = ResponsesUtils::build_output_array(result);
                        json response_obj = ResponsesUtils::build_response_object(
                            response_id,
                            model,
                            output,
                            "completed",
                            result.prompt_tokens,
                            result.completion_tokens,
                            begin_created_at,
                            json(nullptr),
                            json(nullptr),
                            previous_response_id,
                            metadata);
                        add_reasoning_usage_details(response_obj, result);
                        bool completed =
                            ResponseStore::getInstance().completeResponse(
                            response_id,
                            make_assistant_messages(result),
                            output,
                            response_obj,
                            response_obj["usage"]);
                        if (!completed) {
                            std::optional<StoredResponse> stored =
                                ResponseStore::getInstance().getResponse(
                                    response_id);
                            if (stored.has_value()
                                && stored->response_object.is_object()) {
                                response_obj = stored->response_object;
                            }
                        }

                        emit_event("response.completed", {
                            {"type", "response.completed"},
                            {"response", response_obj}
                        });
                    } else {
                        ResponseStore::getInstance().failResponse(
                            response_id,
                            make_store_error_object(
                                http_status >= 500
                                    ? "server_error"
                                    : "invalid_request_error",
                                error_msg));
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
            StandardResponse result =
                scheduler::ModelScheduler::getInstance().runBlocking(
                    standard_request, invoke_options);

            // Build output array.
            // build_output_array() automatically emits a separate top-level
            // {"type":"reasoning"} item when reasoning_content is non-empty,
            // per the OpenAI Responses API spec.
            json output = ResponsesUtils::build_output_array(result);

            // Build response object
            json response_obj = ResponsesUtils::build_response_object(
                response_id, model, output, "completed",
                result.prompt_tokens, result.completion_tokens,
                begin.created_at, json(nullptr), json(nullptr),
                previous_response_id, metadata);

            // Add output_tokens_details.reasoning_tokens when thinking occurred
            add_reasoning_usage_details(response_obj, result);
            bool completed = store.completeResponse(
                response_id,
                make_assistant_messages(result),
                output,
                response_obj,
                response_obj["usage"]);
            if (!completed) {
                std::optional<StoredResponse> stored =
                    store.getResponse(response_id);
                if (stored.has_value()
                    && stored->response_object.is_object()) {
                    response_obj = stored->response_object;
                }
            }

            auto resp = make_json_response(response_obj, k200OK);
            callback(resp);
        }

    } catch (const GenAIException& e) {
        std::string error_type = (e.http_status >= 500) ? "server_error"
                                                         : "invalid_request_error";
        store.failResponse(
            response_id,
            make_store_error_object(error_type, e.message));
        callback(make_error_response(e.http_status, e.message, error_type));
    } catch (const std::exception& e) {
        store.failResponse(
            response_id,
            make_store_error_object(
                "server_error",
                std::string("Internal server error: ") + e.what()));
        callback(make_error_response(500,
            std::string("Internal server error: ") + e.what()));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// POST /v1/responses/input_tokens — Count input tokens
// ─────────────────────────────────────────────────────────────────────────────
void ResponsesController::countInputTokens(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback) {

    json body;
    try {
        body = json::parse(req->getBody());
    } catch (...) {
        callback(make_error_response(
            400,
            "Invalid JSON body",
            "invalid_request_error"));
        return;
    }

    if (!body.is_object()) {
        callback(make_error_response(
            400,
            "Invalid JSON body",
            "invalid_request_error"));
        return;
    }

    if (body.contains("conversation")) {
        callback(make_error_response(
            400,
            "conversation is not supported",
            "invalid_request_error",
            "conversation"));
        return;
    }

    if (body.contains("truncation") && !body["truncation"].is_null()) {
        if (!body["truncation"].is_string()) {
            callback(make_error_response(
                400,
                "invalid value for 'truncation' - expected 'disabled'",
                "invalid_request_error",
                "truncation"));
            return;
        }
        std::string truncation = body["truncation"].get<std::string>();
        if (truncation != "disabled") {
            callback(make_error_response(
                400,
                truncation == "auto"
                    ? "truncation:auto is not supported"
                    : "invalid value for 'truncation' - expected 'disabled'",
                "invalid_request_error",
                "truncation"));
            return;
        }
    }

    std::string model;
    if (!get_optional_string_field(body, "model", model)) {
        callback(make_error_response(
            400,
            "unknown or unresolvable model",
            "invalid_request_error",
            "model"));
        return;
    }

    auto& config_mgr = ModelConfigManager::getInstance();
    if (model.empty()) {
        model = config_mgr.getDefaultModelId();
    }
    if (model.empty() || !config_mgr.validateModel(model)) {
        callback(make_error_response(
            400,
            "unknown or unresolvable model",
            "invalid_request_error",
            "model"));
        return;
    }

    std::string previous_response_id;
    if (!get_optional_string_field(
            body, "previous_response_id", previous_response_id)) {
        callback(make_error_response(
            400,
            "invalid value for 'previous_response_id'",
            "invalid_request_error",
            "previous_response_id"));
        return;
    }

    std::string instructions;
    if (!get_optional_string_field(body, "instructions", instructions)) {
        callback(make_error_response(
            400,
            "invalid value for 'instructions'",
            "invalid_request_error",
            "instructions"));
        return;
    }
    if (instructions.empty()) {
        instructions = ResponsesConstants::DEFAULT_SYSTEM_PROMPT;
    }

    json tools = json::array();
    if (body.contains("tools") && !body["tools"].is_null()) {
        if (!body["tools"].is_array()) {
            callback(make_error_response(
                400,
                "invalid value for 'tools' - expected array",
                "invalid_request_error",
                "tools"));
            return;
        }
        tools = body["tools"];
    }

    json current_messages = body.contains("input")
        ? ResponsesUtils::input_to_messages(body["input"], "")
        : json::array();

    json ancestor_messages = json::array();
    if (!previous_response_id.empty()) {
        BuildCandidateResult walk =
            ResponseStore::getInstance().buildCandidateMessages(
                previous_response_id, current_messages);
        if (!walk.ok) {
            callback(make_error_response(
                walk.http_status,
                walk.error_message,
                "invalid_request_error",
                "previous_response_id"));
            return;
        }
        ancestor_messages = walk.ancestor_messages;
        current_messages = walk.current_request_messages;
        instructions = ResponsesUtils::inject_summary_into_instructions(
            instructions,
            walk.applied_summary);
    }

    int input_tokens = TokenBudgetUtils::count_input_tokens(
        model, ancestor_messages, current_messages, instructions, tools);

    json response = {
        {"object", "response.input_tokens"},
        {"input_tokens", input_tokens}
    };
    auto resp = make_json_response(response, k200OK);
    callback(resp);
}

// ─────────────────────────────────────────────────────────────────────────────
// GET /v1/responses/{response_id}
// ─────────────────────────────────────────────────────────────────────────────
void ResponsesController::getResponse(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    const std::string& response_id) {

    RetrieveStreamParam stream =
        parse_retrieve_stream_param(req->getParameter("stream"));
    if (stream == RetrieveStreamParam::Enabled) {
        callback(make_error_response(
            400,
            "streaming is not supported for the retrieve endpoint",
            "invalid_request_error",
            "stream"));
        return;
    }
    if (stream == RetrieveStreamParam::Invalid) {
        callback(make_error_response(
            400,
            "invalid value for 'stream' - expected true/false/1/0",
            "invalid_request_error",
            "stream"));
        return;
    }

    const auto& params = req->getParameters();
    if (params.find("starting_after") != params.end()) {
        callback(make_error_response(
            400,
            "starting_after requires streaming, which is not supported for retrieve",
            "invalid_request_error",
            "starting_after"));
        return;
    }

    std::optional<StoredResponse> stored =
        ResponseStore::getInstance().getResponse(response_id);
    if (!stored.has_value()) {
        callback(make_error_response(
            404,
            "Response " + response_id + " not found",
            "invalid_request_error"));
        return;
    }

    json response_obj;
    if (stored->status == StoredResponseStatus::InProgress) {
        response_obj = ResponsesUtils::synthesize_in_progress(
            stored->response_id,
            stored->model,
            stored->created_at,
            stored->previous_response_id,
            stored->metadata);
    } else {
        response_obj = stored->response_object;
    }

    auto resp = make_json_response(response_obj, k200OK);
    callback(resp);
}

// ─────────────────────────────────────────────────────────────────────────────
// GET /v1/responses/{response_id}/input_items
// ─────────────────────────────────────────────────────────────────────────────
void ResponsesController::listInputItems(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    const std::string& response_id) {

    int limit = 20;
    if (!parse_input_items_limit(req->getParameter("limit"), limit)) {
        callback(make_error_response(
            400,
            "invalid value for 'limit' - expected integer between 1 and 100",
            "invalid_request_error",
            "limit"));
        return;
    }

    std::string order = "desc";
    if (!parse_input_items_order(req->getParameter("order"), order)) {
        callback(make_error_response(
            400,
            "invalid value for 'order' - expected 'asc' or 'desc'",
            "invalid_request_error",
            "order"));
        return;
    }

    (void)req->getParameter("include");
    std::optional<ResponseStoreJson> items =
        ResponseStore::getInstance().getInputItems(response_id);
    if (!items.has_value()) {
        callback(make_error_response(
            404,
            "response not found",
            "invalid_request_error"));
        return;
    }

    ResponsesUtils::PaginateResult page =
        ResponsesUtils::paginate_input_items(
            items.value(), limit, order, req->getParameter("after"));
    if (!page.ok) {
        callback(make_error_response(
            400,
            page.error_message,
            "invalid_request_error",
            "after"));
        return;
    }

    auto resp = make_json_response(page.envelope, k200OK);
    callback(resp);
}

// ─────────────────────────────────────────────────────────────────────────────
// DELETE /v1/responses/{response_id}
// ─────────────────────────────────────────────────────────────────────────────
void ResponsesController::deleteResponse(
    const HttpRequestPtr& req,
    std::function<void(const HttpResponsePtr&)>&& callback,
    const std::string& response_id) {

    LOG_INFO("[ResponsesController] Delete response: response=" << response_id);
    DeleteCascadeResult result =
        ResponseStore::getInstance().deleteCascade(response_id);

    if (!result.ok) {
        std::string param =
            result.http_status == 409 ? "response_id" : "";
        callback(make_error_response(result.http_status,
                                     result.error_message,
                                     "invalid_request_error",
                                     param));
        return;
    }

    json response = {
        {"id",      response_id},
        {"object",  "response"},
        {"deleted", true}
    };
    auto resp = make_json_response(response, k200OK);
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
    auto resp = make_json_response(response, k200OK);
    callback(resp);
}
