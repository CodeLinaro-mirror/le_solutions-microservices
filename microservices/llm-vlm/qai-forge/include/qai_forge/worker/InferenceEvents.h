// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// InferenceEvents — Public IPC event DTOs used by qai_forge::IGenerativeBackend
//
// These structs are the public, out-of-tree-safe counterparts of the typed
// IPC event structs previously defined inline in the internal
// InferenceProtocol.h header. They are referenced unqualified (global
// namespace) throughout qai-forge's orchestrator/backend call sites — kept
// in the global namespace here deliberately to avoid rename churn across
// every existing in-tree caller.
//
// internal/qai_forge/worker/InferenceProtocol.h now includes this header
// instead of defining these structs itself; it retains the wire-protocol
// machinery (CommandType/ResponseType constants, JSON Lines serialize/
// deserialize helpers, parser functions) that is NOT part of the public
// backend ABI.
// ─────────────────────────────────────────────────────────────────────────────

struct IPCReadyEvent {
    std::string command_id;  // Optional: matches the command that triggered READY
    std::string event_id;    // Optional: matches the EXECUTE event_id
};

struct IPCTokenEvent {
    std::string event_id;
    std::string session_id;  // Continuous-batching slot key; empty in single-slot mode
    std::string content;
};

struct IPCDoneEvent {
    std::string event_id;
    std::string session_id;  // Continuous-batching slot key; empty in single-slot mode
    std::string finish_reason;  // "stop", "length", "error"
};

struct IPCErrorEvent {
    std::string event_id;
    std::string session_id;  // Continuous-batching slot key; empty in single-slot mode
    std::string command_id;
    std::string message;
};
