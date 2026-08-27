// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// McpAgenticLoop.cpp — Autonomous MCP tool-call loop implementation
//
// Drives the inference → tool-call → result → re-infer cycle.
// Submits each inference round through ModelScheduler.
// Calls McpClientRegistry::callTool() for each tool invocation.
// ─────────────────────────────────────────────────────────────────────────────

#include "mcp/McpAgenticLoop.h"
#include "qai_forge/utils/Logger.h"
#include "qai_forge/QaiForge.h"
#include <iostream>
#include <sstream>
#include <chrono>

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────
McpAgenticLoop::McpAgenticLoop(McpClientRegistry& registry, int max_iterations)
    : registry_(registry), max_iterations_(max_iterations) {}

// ─────────────────────────────────────────────────────────────────────────────
// generateCallId
// ─────────────────────────────────────────────────────────────────────────────
std::string McpAgenticLoop::generateCallId() {
    std::ostringstream oss;
    oss << "mcpcall_" << std::to_string(++call_counter_);
    return oss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// executeToolCalls — execute all tool_calls from a StandardResponse
//
// For each tool call:
//   1. Parse the namespaced tool name and arguments
//   2. Optionally emit SSE in_progress event
//   3. Call McpClientRegistry::callTool()
//   4. Optionally emit SSE completed/failed event
//   5. Build role:"tool" message for the next inference turn
//
// Returns a json array of role:"tool" messages.
// ─────────────────────────────────────────────────────────────────────────────
json McpAgenticLoop::executeToolCalls(const json& tool_calls,
                                       std::vector<McpCallRecord>& records,
                                       McpSseEmitter* emitter,
                                       int output_index_start) {
    json tool_result_messages = json::array();

    for (size_t i = 0; i < tool_calls.size(); ++i) {
        const auto& tc = tool_calls[i];
        std::string call_id   = tc.value("id", generateCallId());
        std::string func_name = tc.value("function", json::object()).value("name", "");
        std::string args_str  = tc.value("function", json::object()).value("arguments", "{}");
        int output_index      = output_index_start + static_cast<int>(i);
        LOG_INFO("[McpAgenticLoop] Executing tool call: call_id="
                 << call_id << " name=" << func_name
                 << " output_index=" << output_index);

        auto [server_label, bare_name] = [&func_name]() {
            auto pos = func_name.find("__");
            if (pos == std::string::npos) return std::make_pair(std::string(""), func_name);
            return std::make_pair(func_name.substr(0, pos), func_name.substr(pos + 2));
        }();

        // Parse arguments
        json arguments;
        try {
            arguments = json::parse(args_str);
        } catch (...) {
            arguments = json::object();
        }

        // Emit output_item.added + in_progress SSE event.
        // output_item.added must precede any other event referencing this
        // output_index — OpenAI-SDK streaming clients build their output[]
        // snapshot strictly by appending on output_item.added, so every
        // index referenced later (including this same mcp_call's completed/
        // failed event and the final message's own output_item.added) must
        // already have a slot reserved for it in order.
        if (emitter) {
            json in_progress_item = {
                {"type",         "mcp_call"},
                {"id",           call_id},
                {"server_label", server_label},
                {"name",         bare_name},
                {"arguments",    args_str},
                {"output",       nullptr},
                {"error",        nullptr}
            };

            (*emitter)("response.output_item.added", {
                {"type",         "response.output_item.added"},
                {"output_index", output_index},
                {"item",         in_progress_item}
            });

            (*emitter)("response.mcp_call.in_progress", {
                {"type",         "response.mcp_call.in_progress"},
                {"output_index", output_index},
                {"item",         in_progress_item}
            });
        }

        // Execute the tool call
        McpToolResult result = registry_.callTool(func_name, arguments, call_id, records);
        LOG_INFO("[McpAgenticLoop] Tool call completed: call_id="
                 << call_id << " name=" << func_name
                 << " is_error=" << (result.is_error ? "true" : "false"));

        // Find the record we just added
        const McpCallRecord& record = records.back();

        // Emit completed/failed SSE event, then output_item.done to close
        // out this item's lifecycle before the next item's added event.
        if (emitter) {
            std::string event_type = result.is_error
                ? "response.mcp_call.failed"
                : "response.mcp_call.completed";

            (*emitter)(event_type, {
                {"type",         event_type},
                {"output_index", output_index},
                {"item",         record.to_output_item()}
            });

            (*emitter)("response.output_item.done", {
                {"type",         "response.output_item.done"},
                {"output_index", output_index},
                {"item",         record.to_output_item()}
            });
        }

        // Build tool result message for the next inference turn
        std::string result_content = result.to_string();
        if (result_content.empty()) result_content = "(no output)";

        tool_result_messages.push_back({
            {"role",         "tool"},
            {"tool_call_id", call_id},
            {"name",         func_name},
            {"content",      result_content}
        });

        // Stop on error if configured
        if (stop_on_tool_error_ && result.is_error) {
            std::cerr << "[McpAgenticLoop] Tool call failed and stop_on_tool_error=true, "
                      << "stopping loop\n";
            break;
        }
    }

    return tool_result_messages;
}

// ─────────────────────────────────────────────────────────────────────────────
// emitFinalMessageEvents — output-item lifecycle for the final text message
//
// Must emit output_item.added (reserving this item's slot in the SDK
// client's output[] snapshot, built by appending on each added event) BEFORE
// content_part.added/output_text.delta/output_text.done, all of which index
// into that same snapshot slot by output_index/content_index.
// ─────────────────────────────────────────────────────────────────────────────
void McpAgenticLoop::emitFinalMessageEvents(const McpSseEmitter& emitter,
                                             const std::string& response_id,
                                             int output_index,
                                             const std::string& final_text) {
    emitter("response.output_item.added", {
        {"type",         "response.output_item.added"},
        {"output_index", output_index},
        {"item", {
            {"type",    "message"},
            {"id",      "msg_" + response_id},
            {"role",    "assistant"},
            {"content", json::array()},
            {"status",  "in_progress"}
        }}
    });

    emitter("response.content_part.added", {
        {"type",          "response.content_part.added"},
        {"output_index",  output_index},
        {"content_index", 0},
        {"part",          {{"type", "output_text"}, {"text", ""}}}
    });

    if (!final_text.empty()) {
        emitter("response.output_text.delta", {
            {"type",          "response.output_text.delta"},
            {"output_index",  output_index},
            {"content_index", 0},
            {"delta",         final_text}
        });
    }

    emitter("response.output_text.done", {
        {"type",          "response.output_text.done"},
        {"output_index",  output_index},
        {"content_index", 0},
        {"text",          final_text}
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// run — blocking agentic loop
// ─────────────────────────────────────────────────────────────────────────────
McpLoopResult McpAgenticLoop::run(const CreateChatCompletionRequest& base_request,
                                   const json& mcp_tools,
                                   const std::string& response_id) {
    McpLoopResult loop_result;

    // Build the working request (will be mutated each iteration)
    CreateChatCompletionRequest current_request = base_request;
    current_request.stream = false;
    current_request.tools  = mcp_tools;
    std::string previous_round_response_id =
        base_request.user.value_or("");

    for (int iter = 0; iter < max_iterations_; ++iter) {
        loop_result.iterations = iter + 1;
        LOG_INFO("[McpAgenticLoop] Inference round started: response="
                 << response_id << " iteration=" << loop_result.iterations
                 << " previous=" << previous_round_response_id);

        // Run one inference round
        qai_forge::GenerateOptions invoke_options;
        invoke_options.response_id = response_id;
        invoke_options.previous_response_id = previous_round_response_id;
        if (iter > 0 && !previous_round_response_id.empty()) {
            invoke_options.tool_output_submission = true;
        }

        StandardResponse response =
            qai_forge::QaiForge::getInstance().generate(
                current_request, invoke_options);

        if (response.finish_reason != "tool_calls" || !response.tool_calls.has_value()
            || response.tool_calls.value().empty()) {
            // No tool calls — we're done
            LOG_INFO("[McpAgenticLoop] Inference round completed without tool calls: response="
                     << response_id << " iteration=" << loop_result.iterations
                     << " finish_reason=" << response.finish_reason);
            loop_result.final_response = response;
            return loop_result;
        }
        previous_round_response_id = response_id.empty() ? response.id : response_id;
        LOG_INFO("[McpAgenticLoop] Tool calls requested: response="
                 << response_id << " iteration=" << loop_result.iterations
                 << " tool_call_count=" << response.tool_calls.value().size());

        // Execute all tool calls
        const json& tool_calls = response.tool_calls.value();
        int output_index_start = static_cast<int>(loop_result.call_records.size());
        json tool_result_messages = executeToolCalls(
            tool_calls, loop_result.call_records, nullptr, output_index_start);

        // Build the assistant message with tool_calls for the next turn
        json assistant_msg = {
            {"role",       "assistant"},
            {"content",    ""},
            {"tool_calls", tool_calls}
        };

        // Append assistant message + tool results to the message history
        json new_messages = current_request.messages;
        new_messages.push_back(assistant_msg);
        for (const auto& tr : tool_result_messages) {
            new_messages.push_back(tr);
        }
        current_request.messages = new_messages;
        // Remove tools from subsequent turns (model already knows about them
        // from the session context)
        current_request.tools = mcp_tools;
    }

    // Max iterations reached
    loop_result.truncated = true;
    LOG_WARN("[McpAgenticLoop] Max iterations reached: response="
             << response_id << " max_iterations=" << max_iterations_);
    // Run one final inference without tools to get a text response
    current_request.tools = std::nullopt;
    try {
        qai_forge::GenerateOptions invoke_options;
        invoke_options.response_id = response_id;
        invoke_options.previous_response_id = previous_round_response_id;
        if (!previous_round_response_id.empty()) {
            invoke_options.tool_output_submission = true;
        }

        loop_result.final_response =
            qai_forge::QaiForge::getInstance().generate(
                current_request, invoke_options);
    } catch (...) {
        // If final inference fails, return a synthetic response
        loop_result.final_response.content = "(Response truncated: maximum tool call iterations reached)";
        loop_result.final_response.finish_reason = "stop";
    }

    return loop_result;
}

// ─────────────────────────────────────────────────────────────────────────────
// runStreaming — streaming agentic loop with SSE event emission
// ─────────────────────────────────────────────────────────────────────────────
McpLoopResult McpAgenticLoop::runStreaming(const CreateChatCompletionRequest& base_request,
                                            const json& mcp_tools,
                                            McpSseEmitter emitter,
                                            const std::string& response_id) {
    McpLoopResult loop_result;

    CreateChatCompletionRequest current_request = base_request;
    current_request.stream = false;  // Use blocking internally; we emit SSE manually
    current_request.tools  = mcp_tools;
    std::string previous_round_response_id =
        base_request.user.value_or("");

    for (int iter = 0; iter < max_iterations_; ++iter) {
        loop_result.iterations = iter + 1;
        LOG_INFO("[McpAgenticLoop] Streaming inference round started: response="
                 << response_id << " iteration=" << loop_result.iterations
                 << " previous=" << previous_round_response_id);

        // Run one inference round (blocking)
        qai_forge::GenerateOptions invoke_options;
        invoke_options.response_id = response_id;
        invoke_options.previous_response_id = previous_round_response_id;
        if (iter > 0 && !previous_round_response_id.empty()) {
            invoke_options.tool_output_submission = true;
        }

        StandardResponse response =
            qai_forge::QaiForge::getInstance().generate(
                current_request, invoke_options);

        if (response.finish_reason != "tool_calls" || !response.tool_calls.has_value()
            || response.tool_calls.value().empty()) {
            // Final answer — emit text delta events
            LOG_INFO("[McpAgenticLoop] Streaming inference round completed without tool calls: response="
                     << response_id << " iteration=" << loop_result.iterations
                     << " finish_reason=" << response.finish_reason);
            loop_result.final_response = response;

            std::string final_text = response.content.value_or("");
            int output_index = static_cast<int>(loop_result.call_records.size());
            emitFinalMessageEvents(emitter, response_id, output_index, final_text);

            return loop_result;
        }
        previous_round_response_id = response_id.empty() ? response.id : response_id;
        LOG_INFO("[McpAgenticLoop] Streaming tool calls requested: response="
                 << response_id << " iteration=" << loop_result.iterations
                 << " tool_call_count=" << response.tool_calls.value().size());

        // Execute tool calls with SSE emission
        const json& tool_calls = response.tool_calls.value();
        int output_index_start = static_cast<int>(loop_result.call_records.size());
        json tool_result_messages = executeToolCalls(
            tool_calls, loop_result.call_records, &emitter, output_index_start);

        // Build next turn messages
        json assistant_msg = {
            {"role",       "assistant"},
            {"content",    ""},
            {"tool_calls", tool_calls}
        };

        json new_messages = current_request.messages;
        new_messages.push_back(assistant_msg);
        for (const auto& tr : tool_result_messages) {
            new_messages.push_back(tr);
        }
        current_request.messages = new_messages;
        current_request.tools    = mcp_tools;
    }

    // Max iterations reached
    loop_result.truncated = true;
    LOG_WARN("[McpAgenticLoop] Streaming max iterations reached: response="
             << response_id << " max_iterations=" << max_iterations_);
    current_request.tools = std::nullopt;
    std::string final_text;
    try {
        qai_forge::GenerateOptions invoke_options;
        invoke_options.response_id = response_id;
        invoke_options.previous_response_id = previous_round_response_id;
        if (!previous_round_response_id.empty()) {
            invoke_options.tool_output_submission = true;
        }

        loop_result.final_response =
            qai_forge::QaiForge::getInstance().generate(
                current_request, invoke_options);
        final_text = loop_result.final_response.content.value_or("");
    } catch (...) {
        final_text = "(Response truncated: maximum tool call iterations reached)";
        loop_result.final_response.content = final_text;
        loop_result.final_response.finish_reason = "stop";
    }

    int output_index = static_cast<int>(loop_result.call_records.size());
    emitFinalMessageEvents(emitter, response_id, output_index, final_text);

    return loop_result;
}
