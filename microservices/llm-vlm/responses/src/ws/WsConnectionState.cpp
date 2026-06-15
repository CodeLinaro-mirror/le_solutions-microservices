// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// WsConnectionState.cpp — Per-WebSocket-connection state implementation
// ─────────────────────────────────────────────────────────────────────────────

#include "ws/WsConnectionState.h"
#include "ws/WsProtocol.h"
#include "qai_forge/session/SessionManager.h"

// ─────────────────────────────────────────────────────────────────────────────
// isExpired — check if the connection has exceeded the active timeout
// ─────────────────────────────────────────────────────────────────────────────
bool WsConnectionState::isExpired() const {
    int timeout_minutes = WsProtocol::connection_timeout_minutes();
    auto age = std::chrono::steady_clock::now() - connected_at;
    return age > std::chrono::minutes(timeout_minutes);
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveSession — resolve previous_response_id to a session_id
//
// Fast path: previous_response_id == last_response_id
//   → return last_session_id immediately (session already warm in SessionManager)
//
// Slow path (store=true only): look up in SessionManager by response_id
//   → SessionManager stores sessions by session_id, not response_id
//   → We use the response_id as the session_id (they are the same in our impl)
//
// Not found: return "" (caller sends previous_response_not_found error)
// ─────────────────────────────────────────────────────────────────────────────
std::string WsConnectionState::resolveSession(const std::string& previous_response_id) const {
    if (previous_response_id.empty()) return "";

    // Fast path: connection-local cache hit
    if (previous_response_id == last_response_id && !last_session_id.empty()) {
        return last_session_id;
    }

    // Slow path: only available when store=true
    if (!store) {
        return "";  // ZDR mode — no persisted fallback
    }

    // In our implementation, the session_id is stored as the `user` field
    // in the request, which maps to the response_id for stateful sessions.
    // Try to find the session by using the response_id as the session_id.
    auto session = SessionManager::getInstance().getSession(previous_response_id);
    if (session) {
        return session->session_id;
    }

    return "";  // not found
}
