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
//   strip_tool_call_protocol_text — remove model-internal tool call tags
//   build_output_array()         — StandardResponse + McpCallRecords → output[]
//   build_response_object()      — assemble final Responses API response JSON
//   synthesize_in_progress()     — assemble Retrieve JSON for active responses
//   normalize_input_items()      — Responses API input → input_items list
//   paginate_input_items()       — slice input_items into a list envelope
//   generate_compaction_id()     — generate a unique "cmp_XXXX" ID
//   inject_summary_into_instructions() — append branch summary to instructions
//   build_vlm_runtime_messages() — build model-facing VLM messages
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
// generate_compaction_id — generate a unique "cmp_XXXXXXXXXXXXXXXX" ID
// ─────────────────────────────────────────────────────────────────────────────
std::string generate_compaction_id();

// ─────────────────────────────────────────────────────────────────────────────
// inject_summary_into_instructions — append applied branch summary
//
// @param instructions    Effective system instructions
// @param applied_summary Summary text returned by ResponseStore
// @return                Instructions with summary text appended
// ─────────────────────────────────────────────────────────────────────────────
std::string inject_summary_into_instructions(
    const std::string& instructions,
    const std::string& applied_summary);

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
// build_vlm_runtime_messages — build model-facing messages for VLM inference
//
// Stored Responses input stays OpenAI-shaped (`input_text`, `input_image`).
// VLM runtime receives chat-style parts (`text`, `image_url`) and only the
// current request text plus the current/latest image from the lineage.
//
// @param current_messages  Messages for the current request
// @param ancestor_messages Stored parent-lineage messages used for image fallback
// @return                  Current-turn VLM runtime messages
// ─────────────────────────────────────────────────────────────────────────────
json build_vlm_runtime_messages(
    const json& current_messages,
    const json& ancestor_messages = json::array());

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
// strip_tool_call_protocol_text — remove raw model tool-call protocol blocks
//
// @param text Model answer text that may contain <tool_call>...</tool_call>
// @return     Text with tool-call protocol blocks removed and whitespace trimmed
// ─────────────────────────────────────────────────────────────────────────────
std::string strip_tool_call_protocol_text(const std::string& text);

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
// @param created_at         Unix timestamp captured by the caller
// @param error              Response error object, or null
// @param incomplete_details Incomplete details object, or null
// @param previous_response_id Parent response ID, or empty for root responses
// @param metadata           Request metadata object, or null
// @return                   Complete Responses API response object
// ─────────────────────────────────────────────────────────────────────────────
json build_response_object(const std::string& response_id,
                            const std::string& model,
                            const json& output,
                            const std::string& status,
                            int prompt_tokens,
                            int completion_tokens,
                            int created_at,
                            const json& error,
                            const json& incomplete_details,
                            const std::string& previous_response_id = "",
                            const json& metadata = json::object());

// ─────────────────────────────────────────────────────────────────────────────
// synthesize_in_progress — assemble Retrieve JSON for an active response
//
// @param response_id          The response ID (resp_XXXX)
// @param model                The model ID
// @param created_at           Unix timestamp captured when the response began
// @param previous_response_id Parent response ID, or empty for root responses
// @param metadata             Stored metadata object, or null
// @return                     Minimal in-progress Responses API object
// ─────────────────────────────────────────────────────────────────────────────
json synthesize_in_progress(const std::string& response_id,
                            const std::string& model,
                            int created_at,
                            const std::string& previous_response_id,
                            const json& metadata);

// ─────────────────────────────────────────────────────────────────────────────
// normalize_input_items — convert raw Responses API input to input_items[]
//
// @param response_id The response ID used to stamp stable item ids
// @param raw_input   The request `input` value
// @return            Normalized input_items array
// ─────────────────────────────────────────────────────────────────────────────
json normalize_input_items(const std::string& response_id,
                           const json& raw_input);

// ─────────────────────────────────────────────────────────────────────────────
// PaginateResult — result of input_items list pagination
// ─────────────────────────────────────────────────────────────────────────────
struct PaginateResult {
    bool ok = false;
    json envelope = json::object();
    std::string error_message;
};

// ─────────────────────────────────────────────────────────────────────────────
// paginate_input_items — slice input_items by order, cursor, and limit
//
// @param items Stored input_items array
// @param limit Maximum number of items to return
// @param order "asc" or "desc"
// @param after Optional item id cursor
// @return      List envelope or cursor error
// ─────────────────────────────────────────────────────────────────────────────
PaginateResult paginate_input_items(const json& items,
                                    int limit,
                                    const std::string& order,
                                    const std::string& after);

} // namespace ResponsesUtils
