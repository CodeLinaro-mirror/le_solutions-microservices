// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// WsConnectionState.h — Per-WebSocket-connection state
//
// One WsConnectionState is created when a client connects and destroyed when
// the connection closes. It holds:
//
//   1. Connection identity (UUID, connected_at timestamp)
//   2. Connection-local response transcript and QaiForge turn lineage
//   3. Sequential execution enforcement (response_in_flight atomic flag)
//
// Thread safety:
//   - response_in_flight is atomic — safe to read/write from any thread
//   - All other fields are written only from the Drogon event loop thread
//     (handleNewConnection, handleConnectionClosed) or from the inference
//     thread after response_in_flight is set. The WsResponsesController
//     serialises access via its shared_mutex.
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <chrono>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>

using json = nlohmann::ordered_json;

struct WsStoredResponse {
    json messages = json::array();
    std::string memory_turn_id;
    std::string tool_chain_response_id;
    bool tool_output_pending = false;
};

struct WsConnectionState {
    // ── Connection identity ───────────────────────────────────────────────────
    std::string connection_id;   // UUID assigned at connect time
    std::chrono::steady_clock::time_point connected_at;

    // ── Connection-owned response lineage ────────────────────────────────────
    std::unordered_map<std::string, WsStoredResponse> responses;
    std::string active_response_id;
    std::string last_session_id;
    std::string last_model;

    // ── Sequential execution enforcement ─────────────────────────────────────
    // Only one response.create can be in flight at a time per connection.
    // exchange(true) returns the old value — if it was already true, reject.
    std::atomic<bool> response_in_flight{false};

    // ── Connection timeout ────────────────────────────────────────────────────
    // Returns true if the connection has exceeded the active timeout.
    // Default: 10 minutes (configurable via RESPONSES_WS_TIMEOUT_MINUTES).
    bool isExpired() const;

    std::optional<WsStoredResponse> findResponse(
        const std::string& response_id) const;
    void recordResponse(const std::string& response_id,
                        json messages,
                        std::string memory_turn_id,
                        std::string tool_chain_response_id,
                        bool tool_output_pending);
};
