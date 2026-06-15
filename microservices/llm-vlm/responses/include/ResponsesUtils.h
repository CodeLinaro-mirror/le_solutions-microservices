// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// ResponsesUtils.h — Shared helpers for HTTP and WebSocket Responses API
//
// These functions are used by both ResponsesController (HTTP) and
// WsResponsesController (WebSocket). Extracting them here avoids duplication
// and ensures both transports produce identical output formats.
//
// Functions:
//   input_to_messages()          — Responses API input → messages array
//   extract_mcp_tool_requests()  — parse MCP tool entries from tools[]
//   build_output_array()         — StandardResponse + McpCallRecords → output[]
//   build_response_object()      — assemble final Responses API response JSON
//   current_unix_time()          — current time as Unix timestamp (seconds)
//   generate_response_id()       — generate a unique "resp_XXXX" ID
// ─────────────────────────────────────────────────────────────────────────────

#include "mcp/McpTypes.h"
#include "qai_forge/InternalDTOs.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using json = nlohmann::ordered_json;

// ─────────────────────────────────────────────────────────────────────────────
// McpToolRequest — describes one MCP tool entry from the tools[] array
// ─────────────────────────────────────────────────────────────────────────────
struct McpToolRequest {
    std::string              server_label;   // e.g. "filesystem"
    std::string              server_url;     // optional: dynamic server URL
    std::vector<std::string> allowed_tools;  // empty = all tools allowed
};

namespace ResponsesUtils {

// ─────────────────────────────────────────────────────────────────────────────
// current_unix_time — current time as Unix timestamp (seconds since epoch)
// ─────────────────────────────────────────────────────────────────────────────
int current_unix_time();

// ─────────────────────────────────────────────────────────────────────────────
// generate_response_id — generate a unique "resp_XXXXXXXXXXXXXXXX" ID
// ─────────────────────────────────────────────────────────────────────────────
std::string generate_response_id();

// ─────────────────────────────────────────────────────────────────────────────
// input_to_messages — convert Responses API `input` to messages array
//
// Handles all Responses API input formats:
//   1. String: "Hello" → [{role:"user", content:"Hello"}]
//   2. Array of message objects: [{role:"user", content:"..."}]
//   3. Array with function_call_output items (tool results from previous turn)
//   4. Array with function_call items (assistant tool calls from previous turn)
//   5. Content parts: [{type:"input_text", text:"..."}]
//
// @param input          The `input` field from the Responses API request
// @param system_prompt  Optional system prompt (from `instructions` field)
// @return               OpenAI-format messages array
// ─────────────────────────────────────────────────────────────────────────────
json input_to_messages(const json& input, const std::string& system_prompt = "");

// ─────────────────────────────────────────────────────────────────────────────
// extract_mcp_tool_requests — parse MCP tool entries from tools[] array
//
// Extracts all {type:"mcp"} entries and returns them as McpToolRequest structs.
// Non-MCP tool entries are ignored (handled separately by the caller).
//
// @param tools  The `tools` array from the Responses API request
// @return       Vector of McpToolRequest (one per MCP server reference)
// ─────────────────────────────────────────────────────────────────────────────
std::vector<McpToolRequest> extract_mcp_tool_requests(const json& tools);

// ─────────────────────────────────────────────────────────────────────────────
// build_output_array — build Responses API output[] from inference results
//
// Combines MCP call records (which happened before the final answer) with
// the final StandardResponse into the Responses API output[] format.
//
// @param result       The final StandardResponse from ChatOrchestrator
// @param mcp_records  MCP tool call records (may be empty for non-MCP requests)
// @return             Responses API output[] array
// ─────────────────────────────────────────────────────────────────────────────
json build_output_array(const StandardResponse& result,
                         const std::vector<McpCallRecord>& mcp_records = {});

// ─────────────────────────────────────────────────────────────────────────────
// build_response_object — assemble the complete Responses API response JSON
//
// @param response_id        The response ID (resp_XXXX)
// @param model              The model ID
// @param output             The output[] array (from build_output_array)
// @param status             "completed" | "incomplete" | "cancelled"
// @param prompt_tokens      Token count for the prompt
// @param completion_tokens  Token count for the completion
// @param truncated          true if max_tool_calls was reached
// @return                   Complete Responses API response object
// ─────────────────────────────────────────────────────────────────────────────
json build_response_object(const std::string& response_id,
                            const std::string& model,
                            const json& output,
                            const std::string& status,
                            int prompt_tokens,
                            int completion_tokens,
                            bool truncated = false);

} // namespace ResponsesUtils
