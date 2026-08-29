// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// WsConnectionState.cpp — Per-WebSocket-connection state implementation
// ─────────────────────────────────────────────────────────────────────────────

#include "ws/WsConnectionState.h"
#include "ws/WsProtocol.h"

// ─────────────────────────────────────────────────────────────────────────────
// isExpired — check if the connection has exceeded the active timeout
// ─────────────────────────────────────────────────────────────────────────────
bool WsConnectionState::isExpired() const {
    int timeout_minutes = WsProtocol::connection_timeout_minutes();
    auto age = std::chrono::steady_clock::now() - connected_at;
    return age > std::chrono::minutes(timeout_minutes);
}

std::optional<WsStoredResponse> WsConnectionState::findResponse(
    const std::string& response_id) const {
    const auto found = responses.find(response_id);
    if (found == responses.end()) {
        return std::nullopt;
    }
    return found->second;
}

void WsConnectionState::recordResponse(const std::string& response_id,
                                       json messages,
                                       std::string memory_turn_id,
                                       std::string tool_chain_response_id,
                                       bool tool_output_pending) {
    responses[response_id] = {
        std::move(messages),
        std::move(memory_turn_id),
        std::move(tool_chain_response_id),
        tool_output_pending,
    };
}
