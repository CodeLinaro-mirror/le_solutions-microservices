// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// McpAgenticLoop.cpp — Autonomous MCP tool-call loop implementation
//
// Drives the inference → tool-call → result → re-infer cycle.
// Calls ChatOrchestrator::handleBlocking() (Layer 2) for each inference round.
// Calls McpClientRegistry::callTool() for each tool invocation.
// ─────────────────────────────────────────────────────────────────────────────

#include "mcp/McpAgenticLoop.h"
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

        // Parse arguments
        json arguments;
        try {
            arguments = json::parse(args_str);
        } catch (...) {
            arguments = json::object();
        }

        // Emit in_progress SSE event
        if (emitter) {
            auto [server_label, bare_name] = [&func_name]() {
                auto pos = func_name.find("__");
                if (pos == std::string::npos) return std::make_pair(std::string(""), func_name);
                return std::make_pair(func_name.substr(0, pos), func_name.substr(pos + 2));
            }();

            (*emitter)("response.mcp_call.in_progress", {
                {"type",         "response.mcp_call.in_progress"},
                {"output_index", output_index},
                {"item", {
                    {"type",         "mcp_call"},
                    {"id",           call_id},
                    {"server_label", server_label},
                    {"name",         bare_name},
                    {"arguments",    args_str}
                }}
            });
        }

        // Execute the tool call
        McpToolResult result = registry_.callTool(func_name, arguments, call_id, records);

        // Find the record we just added
        const McpCallRecord& record = records.back();

        // Emit completed/failed SSE event
        if (emitter) {
            std::string event_type = result.is_error
                ? "response.mcp_call.failed"
                : "response.mcp_call.completed";

            (*emitter)(event_type, {
                {"type",         event_type},
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
// run — blocking agentic loop
// ─────────────────────────────────────────────────────────────────────────────
McpLoopResult McpAgenticLoop::run(const CreateChatCompletionRequest& base_request,
                                   const json& mcp_tools) {
    McpLoopResult loop_result;

    // Build the working request (will be mutated each iteration)
    CreateChatCompletionRequest current_request = base_request;
    current_request.stream = false;
    current_request.tools  = mcp_tools;

    for (int iter = 0; iter < max_iterations_; ++iter) {
        loop_result.iterations = iter + 1;

        // Run one inference round
        qai_forge::GenerateOptions opts;
        opts.session_id = current_request.user.value_or("");
        StandardResponse response =
            qai_forge::QaiForge::getInstance().generate(current_request, opts);

        if (response.finish_reason != "tool_calls" || !response.tool_calls.has_value()
            || response.tool_calls.value().empty()) {
            // No tool calls — we're done
            loop_result.final_response = response;
            return loop_result;
        }

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
    // Run one final inference without tools to get a text response
    current_request.tools = std::nullopt;
    try {
        qai_forge::GenerateOptions opts;
        opts.session_id = current_request.user.value_or("");
        loop_result.final_response =
            qai_forge::QaiForge::getInstance().generate(current_request, opts);
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

    for (int iter = 0; iter < max_iterations_; ++iter) {
        loop_result.iterations = iter + 1;

        // Run one inference round (blocking)
        qai_forge::GenerateOptions opts;
        opts.session_id = current_request.user.value_or("");
        StandardResponse response =
            qai_forge::QaiForge::getInstance().generate(current_request, opts);

        if (response.finish_reason != "tool_calls" || !response.tool_calls.has_value()
            || response.tool_calls.value().empty()) {
            // Final answer — emit text delta events
            loop_result.final_response = response;

            std::string final_text = response.content.value_or("");
            int output_index = static_cast<int>(loop_result.call_records.size());

            // Emit content_part.added
            emitter("response.content_part.added", {
                {"type",          "response.content_part.added"},
                {"output_index",  output_index},
                {"content_index", 0},
                {"part",          {{"type", "output_text"}, {"text", ""}}}
            });

            // Emit the full text as a single delta (could be chunked in future)
            if (!final_text.empty()) {
                emitter("response.output_text.delta", {
                    {"type",          "response.output_text.delta"},
                    {"output_index",  output_index},
                    {"content_index", 0},
                    {"delta",         final_text}
                });
            }

            // Emit output_text.done
            emitter("response.output_text.done", {
                {"type",          "response.output_text.done"},
                {"output_index",  output_index},
                {"content_index", 0},
                {"text",          final_text}
            });

            return loop_result;
        }

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
    current_request.tools = std::nullopt;
    try {
        qai_forge::GenerateOptions opts;
        opts.session_id = current_request.user.value_or("");
        loop_result.final_response =
            qai_forge::QaiForge::getInstance().generate(current_request, opts);
        std::string final_text = loop_result.final_response.content.value_or("");
        int output_index = static_cast<int>(loop_result.call_records.size());

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
    } catch (...) {
        loop_result.final_response.content = "(Response truncated: maximum tool call iterations reached)";
        loop_result.final_response.finish_reason = "stop";
    }

    return loop_result;
}
