// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// ResponsesUtils.cpp — Shared helpers for HTTP and WebSocket Responses API
//
// Implements the utility functions declared in ResponsesUtils.h.
// Used by both ResponsesController (HTTP) and WsResponsesController (WebSocket).
// ─────────────────────────────────────────────────────────────────────────────

#include "ResponsesUtils.h"
#include <chrono>
#include <sstream>
#include <iomanip>
#include <random>

namespace ResponsesUtils {

// ─────────────────────────────────────────────────────────────────────────────
// current_unix_time
// ─────────────────────────────────────────────────────────────────────────────
int current_unix_time() {
    return static_cast<int>(
        std::chrono::system_clock::now().time_since_epoch().count() / 1000000000LL);
}

// ─────────────────────────────────────────────────────────────────────────────
// generate_response_id
// ─────────────────────────────────────────────────────────────────────────────
std::string generate_response_id() {
    static std::mt19937_64 rng(std::random_device{}());
    std::ostringstream oss;
    oss << "resp_" << std::hex << std::setw(16) << std::setfill('0') << rng();
    return oss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// input_to_messages
//
// Converts Responses API `input` to OpenAI-format messages array.
// Handles all input formats defined in the Responses API spec.
// ─────────────────────────────────────────────────────────────────────────────
json input_to_messages(const json& input, const std::string& system_prompt) {
    json messages = json::array();

    if (!system_prompt.empty()) {
        messages.push_back({{"role", "system"}, {"content", system_prompt}});
    }

    if (input.is_string()) {
        messages.push_back({{"role", "user"}, {"content", input.get<std::string>()}});
        return messages;
    }

    if (!input.is_array()) return messages;

    for (const auto& item : input) {
        if (item.is_string()) {
            messages.push_back({{"role", "user"}, {"content", item.get<std::string>()}});
            continue;
        }
        if (!item.is_object()) continue;

        std::string item_type = item.value("type", "");

        // function_call_output — tool result from a previous turn
        if (item_type == "function_call_output") {
            messages.push_back({
                {"role",         "tool"},
                {"tool_call_id", item.value("call_id", "")},
                {"content",      item.value("output", "")}
            });
            continue;
        }

        // function_call — assistant tool call from a previous turn
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

        // Standard message or content part. Preserve content arrays as-is so
        // VLM image_url parts survive the shared HTTP/WebSocket conversion.
        std::string role = item.value("role", "user");
        if (item.contains("content")) {
            messages.push_back({{"role", role}, {"content", item["content"]}});
        } else if (item.contains("text")) {
            messages.push_back({{"role", role}, {"content", item["text"]}});
        }
    }

    return messages;
}

// ─────────────────────────────────────────────────────────────────────────────
// extract_mcp_tool_requests
// ─────────────────────────────────────────────────────────────────────────────
std::vector<McpToolRequest> extract_mcp_tool_requests(const json& tools) {
    std::vector<McpToolRequest> requests;
    if (!tools.is_array()) return requests;

    for (const auto& tool : tools) {
        if (!tool.is_object()) continue;
        if (tool.value("type", "") != "mcp") continue;

        McpToolRequest req;
        req.server_label = tool.value("server_label", "");
        req.server_url   = tool.value("server_url", "");

        if (tool.contains("allowed_tools") && tool["allowed_tools"].is_array()) {
            for (const auto& t : tool["allowed_tools"]) {
                if (t.is_string()) req.allowed_tools.push_back(t.get<std::string>());
            }
        }
        requests.push_back(req);
    }
    return requests;
}

// ─────────────────────────────────────────────────────────────────────────────
// build_output_array
//
// Per the OpenAI Responses API spec, the reasoning output item is a SEPARATE
// top-level item in output[], not nested inside message.content[].
//
// Correct structure:
//   output[0] = {"type":"reasoning", "id":"rs_...", "summary":[...]}  ← separate
//   output[1] = {"type":"message", "content":[{"type":"output_text"}]} ← answer only
//
// The reasoning item always appears when reasoning_content is non-empty,
// regardless of whether reasoning.summary was requested in the API call.
// ─────────────────────────────────────────────────────────────────────────────
json build_output_array(const StandardResponse& result,
                         const std::vector<McpCallRecord>& mcp_records) {
    json output = json::array();

    // MCP call records come first (they happened before the final answer)
    for (const auto& record : mcp_records) {
        output.push_back(record.to_output_item());
    }

    // ── Reasoning output item (separate top-level item, NOT inside message) ──
    // Emitted whenever the model produced thinking tokens, regardless of whether
    // the client requested a summary. The full thinking text is in summary[].
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

    // ── Message output item (answer text only — no reasoning here) ────────────
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
// build_response_object
// ─────────────────────────────────────────────────────────────────────────────
json build_response_object(const std::string& response_id,
                            const std::string& model,
                            const json& output,
                            const std::string& status,
                            int prompt_tokens,
                            int completion_tokens,
                            bool truncated) {
    return {
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
}

} // namespace ResponsesUtils
