// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// WsResponsesController.h — WebSocket Responses API controller
//
// Implements the WebSocket endpoint:
//   ws://host:9002/v1/responses/ws
//
// Runs inside the same Drogon instance as the HTTP ResponsesController.
// Drogon handles the HTTP→WebSocket upgrade automatically.
//
// Client → Server: only "response.create" events
// Server → Client: response.created, response.output_text.delta,
//                  response.mcp_call.*, response.completed, error
//
// Key design properties:
//   - One WsConnectionState per connection (connection-local response cache)
//   - Sequential execution: one response.create in flight at a time
//   - 10-minute connection timeout (RESPONSES_WS_TIMEOUT_MINUTES env var)
//   - Inference runs on a detached thread to avoid blocking Drogon's event loop
//   - McpAgenticLoop::runStreaming() is reused with a WS emitter callback
//   - Connection-scoped QaiForge memory released on disconnect
// ─────────────────────────────────────────────────────────────────────────────

#include "ws/WsConnectionState.h"
#include "ws/WsProtocol.h"
#include "ResponsesUtils.h"
#include "mcp/McpClientRegistry.h"
#include "mcp/McpAgenticLoop.h"
#include "qai_forge/InternalDTOs.h"
#include <drogon/WebSocketController.h>
#include <nlohmann/json.hpp>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <string>

using json = nlohmann::ordered_json;

class WsResponsesController
    : public drogon::WebSocketController<WsResponsesController> {
public:
    WS_PATH_LIST_BEGIN
        WS_PATH_ADD("/v1/responses/ws");
    WS_PATH_LIST_END

    // ── Drogon WebSocket callbacks ────────────────────────────────────────────

    /**
     * Called by Drogon when a new WebSocket connection is established
     * (after the HTTP Upgrade handshake completes).
     * Creates a WsConnectionState and stores it in the connection map.
     */
    void handleNewConnection(const drogon::HttpRequestPtr& req,
                             const drogon::WebSocketConnectionPtr& conn) override;

    /**
     * Called by Drogon for each incoming WebSocket text or binary frame.
     * Parses the JSON event and dispatches to the appropriate handler.
     * Checks the 10-minute connection timeout before processing.
     */
    void handleNewMessage(const drogon::WebSocketConnectionPtr& conn,
                          std::string&& message,
                          const drogon::WebSocketMessageType& type) override;

    /**
     * Called by Drogon when the WebSocket connection closes (client disconnect,
     * server shutdown, or timeout). Cleans up the WsConnectionState.
     */
    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override;

private:
    // ── Event handlers ────────────────────────────────────────────────────────

    /**
     * Handle a "response.create" event from the client.
     * Validates the request, resolves the session, and spawns a detached
     * inference thread via runResponse().
     */
    void onResponseCreate(const drogon::WebSocketConnectionPtr& conn,
                          const std::shared_ptr<WsConnectionState>& state,
                          const json& event);

    // ── Inference execution ───────────────────────────────────────────────────

    /**
     * Run the full response generation on a detached thread.
     * This must NOT run on Drogon's event loop thread — inference blocks
     * for the duration of the model's output.
     *
     * Execution path:
     *   1. Send response.created event
     *   2. Send response.output_item.added event
     *   3a. If MCP tools: McpAgenticLoop::runStreaming() with WS emitter
     *   3b. Else: ChatOrchestrator::handleStreaming() with WS emitter
     *   4. Send response.output_text.done + response.output_item.done
     *   5. Send response.completed
     *   6. Update connection-owned transcript and memory lineage
     *   7. Set response_in_flight = false
     *
     * @param conn              WebSocket connection (shared_ptr — keeps alive)
     * @param state             Connection-owned transcript and lineage state
     * @param sdk_request       Parsed CreateChatCompletionRequest
     * @param mcp_function_tools OpenAI-format function tools (translated from MCP)
     * @param has_mcp_tools     true if MCP tools are present
     * @param response_id       Pre-generated response ID (resp_XXXX)
     * @param model             Model ID (for event payloads)
     * @param memory_parent_turn_id Nearest generated QaiForge memory parent
     * @param tool_chain_response_id Originating response for tool continuation
     * @param tool_output_submission Whether retained input contains tool output
     */
    void runResponse(drogon::WebSocketConnectionPtr conn,
                     std::shared_ptr<WsConnectionState> state,
                     CreateChatCompletionRequest sdk_request,
                     json mcp_function_tools,
                     bool has_mcp_tools,
                     std::string response_id,
                     std::string model,
                     std::string memory_parent_turn_id,
                     std::string tool_chain_response_id,
                     bool tool_output_submission);

    // ── Warmup (generate=false) ───────────────────────────────────────────────

    /**
     * Handle generate=false by retaining input without running inference.
     * Sends response.created + response.completed with empty output[].
     */
    void runWarmup(const drogon::WebSocketConnectionPtr& conn,
                   const std::shared_ptr<WsConnectionState>& state,
                   json messages,
                   const std::string& response_id,
                   const std::string& model,
                   std::string memory_parent_turn_id,
                   std::string tool_chain_response_id,
                   bool tool_output_pending);

    // ── Utilities ─────────────────────────────────────────────────────────────

    /**
     * Send a JSON event as a WebSocket text frame.
     * Thread-safe: Drogon's WebSocketConnection::send() is thread-safe.
     */
    static void sendEvent(const drogon::WebSocketConnectionPtr& conn,
                          const json& event);

    /**
     * Get the connection ID stored in the connection's context.
     * Returns "" if not set (should not happen in normal operation).
     */
    static std::string getConnectionId(const drogon::WebSocketConnectionPtr& conn);

    // ── Connection state map ──────────────────────────────────────────────────
    // Maps connection_id → WsConnectionState.
    // Protected by shared_mutex: multiple readers (sendEvent, runResponse)
    // and exclusive writers (handleNewConnection, handleConnectionClosed,
    // state updates after inference).
    mutable std::shared_mutex                              mutex_;
    std::unordered_map<std::string, std::shared_ptr<WsConnectionState>> states_;
};
