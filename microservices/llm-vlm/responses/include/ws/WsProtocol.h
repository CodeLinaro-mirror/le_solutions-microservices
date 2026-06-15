// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// WsProtocol.h — WebSocket Responses API event type constants and JSON builders
//
// All event type strings are defined here as constexpr to avoid magic strings
// scattered across the codebase. The make_error() helper builds the standard
// error event JSON used by both the WS controller and connection state.
//
// Server event names are identical to the HTTP SSE event names — the only
// difference is that WS sends raw JSON text frames instead of SSE-formatted
// "event: ...\ndata: ...\n\n" lines.
// ─────────────────────────────────────────────────────────────────────────────

#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::ordered_json;

namespace WsProtocol {

// ─────────────────────────────────────────────────────────────────────────────
// Client → Server event types
// ─────────────────────────────────────────────────────────────────────────────
constexpr const char* CLIENT_RESPONSE_CREATE = "response.create";

// ─────────────────────────────────────────────────────────────────────────────
// Server → Client event types
// (identical names to HTTP SSE events)
// ─────────────────────────────────────────────────────────────────────────────
constexpr const char* SERVER_RESPONSE_CREATED          = "response.created";
constexpr const char* SERVER_RESPONSE_IN_PROGRESS      = "response.in_progress";
constexpr const char* SERVER_OUTPUT_ITEM_ADDED         = "response.output_item.added";
constexpr const char* SERVER_CONTENT_PART_ADDED        = "response.content_part.added";
constexpr const char* SERVER_OUTPUT_TEXT_DELTA         = "response.output_text.delta";
constexpr const char* SERVER_REASONING_DELTA           = "response.reasoning.delta";
constexpr const char* SERVER_OUTPUT_TEXT_DONE          = "response.output_text.done";
constexpr const char* SERVER_OUTPUT_ITEM_DONE          = "response.output_item.done";
constexpr const char* SERVER_MCP_CALL_IN_PROGRESS      = "response.mcp_call.in_progress";
constexpr const char* SERVER_MCP_CALL_COMPLETED        = "response.mcp_call.completed";
constexpr const char* SERVER_MCP_CALL_FAILED           = "response.mcp_call.failed";
constexpr const char* SERVER_RESPONSE_COMPLETED        = "response.completed";
constexpr const char* SERVER_ERROR                     = "error";

// ─────────────────────────────────────────────────────────────────────────────
// Error codes
// ─────────────────────────────────────────────────────────────────────────────
constexpr const char* ERR_RESPONSE_IN_PROGRESS         = "response_in_progress";
constexpr const char* ERR_CONNECTION_LIMIT_REACHED     = "websocket_connection_limit_reached";
constexpr const char* ERR_PREVIOUS_RESPONSE_NOT_FOUND  = "previous_response_not_found";
constexpr const char* ERR_MCP_SERVER_NOT_FOUND         = "mcp_server_not_found";
constexpr const char* ERR_MODEL_NOT_FOUND              = "model_not_found";
constexpr const char* ERR_INFERENCE_FAILED             = "inference_failed";
constexpr const char* ERR_UNKNOWN_EVENT_TYPE           = "unknown_event_type";
constexpr const char* ERR_INVALID_REQUEST              = "invalid_request_error";
constexpr const char* ERR_SERVER_ERROR                 = "server_error";

// ─────────────────────────────────────────────────────────────────────────────
// make_error — build a standard error event JSON object
//
// @param status   HTTP-equivalent status code (400, 404, 500, etc.)
// @param code     Machine-readable error code (one of ERR_* constants)
// @param message  Human-readable error message
// @param param    Optional: the request field that caused the error
// @return         JSON error event ready to send as a WS text frame
// ─────────────────────────────────────────────────────────────────────────────
json make_error(int status,
                const std::string& code,
                const std::string& message,
                const std::string& param = "");

// ─────────────────────────────────────────────────────────────────────────────
// Connection limit timeout in minutes (default 10, override via env var)
// ─────────────────────────────────────────────────────────────────────────────
int connection_timeout_minutes();

} // namespace WsProtocol
