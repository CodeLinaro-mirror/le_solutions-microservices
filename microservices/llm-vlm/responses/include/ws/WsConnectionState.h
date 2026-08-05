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
//   2. Connection-local response cache (last_response_id / last_session_id)
//      — the key latency optimization: when previous_response_id matches
//        last_response_id, the session is already warm in SessionManager
//   3. Sequential execution enforcement (response_in_flight atomic flag)
//   4. ZDR / store=false mode flag
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
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// WsConnectionState — Rule of Five
//
// std::atomic<bool> is neither copyable nor movable by default (its copy/move
// assignment operators are deleted). This means the compiler cannot synthesise
// a move constructor or move assignment operator for WsConnectionState.
//
// We apply the Rule of Five: since we need move semantics (for insertion into
// the std::unordered_map in WsResponsesController), we explicitly define:
//   - Move constructor       — loads the atomic value from the source
//   - Move assignment        — loads the atomic value from the source
//   - Copy constructor       — deleted (connections are not copyable)
//   - Copy assignment        — deleted (connections are not copyable)
//   - Destructor             — defaulted (no manual resource management)
// ─────────────────────────────────────────────────────────────────────────────
struct WsConnectionState {
    // ── Connection identity ───────────────────────────────────────────────────
    std::string connection_id;   // UUID assigned at connect time
    std::chrono::steady_clock::time_point connected_at;

    // ── Connection-local response cache ──────────────────────────────────────
    // Holds the most recent response on this connection.
    // When previous_response_id == last_response_id, the session is already
    // warm in SessionManager — no lookup needed (fast path).
    std::string last_response_id;   // e.g. "resp_abc123"
    std::string last_session_id;    // corresponding SessionManager session_id

    // ── Sequential execution enforcement ─────────────────────────────────────
    // Only one response.create can be in flight at a time per connection.
    // exchange(true) returns the old value — if it was already true, reject.
    std::atomic<bool> response_in_flight{false};

    // ── ZDR / store=false mode ────────────────────────────────────────────────
    // When store=false, previous_response_id resolution only checks the
    // connection-local cache. If not found, returns previous_response_not_found.
    bool store = true;

    // ── Rule of Five ─────────────────────────────────────────────────────────

    WsConnectionState() = default;
    ~WsConnectionState() = default;

    // Not copyable — each connection is a unique resource
    WsConnectionState(const WsConnectionState&) = delete;
    WsConnectionState& operator=(const WsConnectionState&) = delete;

    // Movable — required for std::unordered_map::operator[]
    WsConnectionState(WsConnectionState&& other) noexcept
        : connection_id(std::move(other.connection_id))
        , connected_at(other.connected_at)
        , last_response_id(std::move(other.last_response_id))
        , last_session_id(std::move(other.last_session_id))
        , response_in_flight(other.response_in_flight.load())
        , store(other.store)
    {}

    WsConnectionState& operator=(WsConnectionState&& other) noexcept {
        if (this != &other) {
            connection_id      = std::move(other.connection_id);
            connected_at       = other.connected_at;
            last_response_id   = std::move(other.last_response_id);
            last_session_id    = std::move(other.last_session_id);
            response_in_flight.store(other.response_in_flight.load());
            store              = other.store;
        }
        return *this;
    }

    // ── Connection timeout ────────────────────────────────────────────────────
    // Returns true if the connection has exceeded the active timeout.
    // Default: 10 minutes (configurable via RESPONSES_WS_TIMEOUT_MINUTES).
    bool isExpired() const;

    // ── Session resolution ────────────────────────────────────────────────────
    // Resolve a previous_response_id to a SessionManager session_id.
    //
    // Resolution order:
    //   1. Fast path: previous_response_id == last_response_id
    //      → return last_session_id immediately (no lookup)
    //   2. Slow path (store=true only): SessionManager::getSession(previous_response_id)
    //      → if found, return its session_id
    //   3. Not found: return "" (caller sends previous_response_not_found error)
    //
    // @param previous_response_id  The ID from the client's response.create event
    // @return                      session_id, or "" if not found
    std::string resolveSession(const std::string& previous_response_id) const;
};
