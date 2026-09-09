// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <string>
#include <nlohmann/json.hpp>

using json = nlohmann::ordered_json;

// nlohmann::json's .value(key, default) only falls back to `default` when the
// key is absent — if the key is present but explicitly null, .value<std::string>()
// throws json::type_error.302. This inline helper treats an explicit null the
// same as an absent key. Defined here (not in an anonymous namespace, since
// this is a header) with an "ipc" prefix to avoid ODR / name-collision issues.
inline std::string ipcGetStringOrDefault(const json& obj,
                                          const std::string& key,
                                          const std::string& def = "") {
    if (!obj.is_object() || !obj.contains(key) || obj[key].is_null()) {
        return def;
    }
    const json& val = obj[key];
    return val.is_string() ? val.get<std::string>() : def;
}

// ─────────────────────────────────────────────────────────────────────────────
// InferenceProtocol — Layer 3 IPC Message Definitions
//
// Defines the JSON Lines protocol used between the Drogon server (Layer 3
// InferenceWorkerManager) and the genai-inference-worker subprocess.
//
// Design decisions (architecture_refactoring_design.md Section 4.B):
//   - All messages are typed via the "type" field.
//   - Typed IPC event structs replace raw dictionary parsing.
//   - The protocol is identical to the Python implementation, ensuring
//     the C++ worker is a drop-in replacement for llm_process.py.
//
// Command flow:
//   Server → Worker: INIT, EXECUTE, RESET, SAVE_KV, RESTORE_KV, SHUTDOWN
//   Worker → Server: READY, TOKEN, DONE, ERROR
//
// EXECUTE's prompt text does not travel inline in this JSON. The server
// memcpy's it into a memfd-backed shared-memory region (inherited by the
// worker across fork()/exec() via the PROMPT_SHM_FD/PROMPT_SHM_BYTES env
// vars — see InferenceWorkerManager::startWorker()) and EXECUTE carries only
// a "prompt_ref": {"offset":..,"len":..} pointing into it. This avoids the
// JSON-escape/unescape scan a large inline prompt string would otherwise
// cost on every request. Streamed TOKEN output is unaffected — token chunks
// are small and stay inline.
// ─────────────────────────────────────────────────────────────────────────────

// ── Command types (Server → Worker) ──────────────────────────────────────────
namespace CommandType {
    constexpr const char* INIT          = "INIT";
    constexpr const char* EXECUTE       = "EXECUTE";
    constexpr const char* RESET         = "RESET";
    constexpr const char* SAVE_KV       = "SAVE_KV";
    constexpr const char* RESTORE_KV    = "RESTORE_KV";
    constexpr const char* SHUTDOWN      = "SHUTDOWN";
    constexpr const char* CLEAR_SESSION = "CLEAR_SESSION";
}
// ── Response types (Worker → Server) ─────────────────────────────────────────
namespace ResponseType {
    constexpr const char* READY = "READY";
    constexpr const char* TOKEN = "TOKEN";
    constexpr const char* DONE  = "DONE";
    constexpr const char* ERROR = "ERROR";
}

// ─────────────────────────────────────────────────────────────────────────────
// Typed IPC Event Structs (Section 4.B — DTOs at the IPC Boundary)
//
// These replace raw dictionary parsing (response["type"]) with strongly-typed
// objects. The async socket loop yields these directly.
// ─────────────────────────────────────────────────────────────────────────────

struct IPCReadyEvent {
    std::string command_id;  // Optional: matches the command that triggered READY
    std::string event_id;    // Optional: matches the EXECUTE event_id
};

struct IPCTokenEvent {
    std::string event_id;
    std::string content;
};

struct IPCDoneEvent {
    std::string event_id;
    std::string finish_reason;  // "stop", "length", "error"
};

struct IPCErrorEvent {
    std::string event_id;
    std::string command_id;
    std::string message;
};

// ─────────────────────────────────────────────────────────────────────────────
// InferenceProtocol — Serialization / Deserialization helpers
// ─────────────────────────────────────────────────────────────────────────────
class InferenceProtocol {
public:
    // ── Serialize a JSON message to a JSON Line (appends '\n') ────────────────
    static std::string serialize(const json& msg) {
        return msg.dump() + "\n";
    }

    // ── Deserialize a JSON Line to a json object ──────────────────────────────
    static json deserialize(const std::string& line) {
        return json::parse(line);
    }

    // ── Command factory methods ───────────────────────────────────────────────

    static json createInitCommand(const std::string& model_id,
                                   const std::string& config_file,
                                   const std::string& sampler_config,
                                   bool streaming = true) {
        return {
            {"type", CommandType::INIT},
            {"model", model_id},
            {"config_file", config_file},
            {"sampler_config", sampler_config},
            {"streaming", streaming}
        };
    }

    // prompt_ref: {"offset":.., "len":..} into the prompt shared-memory
    // region (see InferenceWorkerManager::writePromptToShm) — the prompt
    // text itself never travels inline in this JSON message.
    static json createExecuteCommand(const std::string& event_id,
                                      const json& prompt_ref,
                                      bool streaming,
                                      int max_tokens = 1024,
                                      float temperature = 1.0f,
                                      float top_p = 1.0f,
                                      int top_k = 40,
                                      float presence_penalty = 0.0f,
                                      float frequency_penalty = 0.0f,
                                      bool bypass_think_filter = false,
                                      const std::string& session_id = "",
                                      bool kv_invalidated = false) {
        json cmd = {
            {"type", CommandType::EXECUTE},
            {"event_id", event_id},
            {"prompt_ref", prompt_ref},
            {"streaming", streaming},
            {"max_tokens", max_tokens},
            {"temperature", temperature},
            {"top_p", top_p},
            {"top_k", top_k},
            {"presence_penalty", presence_penalty},
            {"frequency_penalty", frequency_penalty},
            {"bypass_think_filter", bypass_think_filter}
        };
        if (!session_id.empty()) {
            cmd["session_id"] = session_id;
        }
        if (kv_invalidated) {
            cmd["kv_invalidated"] = true;
        }
        return cmd;
    }

    static json createStructuredExecuteCommand(
        const std::string& event_id,
        const json& messages,
        const json& tools,
        bool streaming,
        int max_tokens = 1024,
        float temperature = 1.0f,
        float top_p = 1.0f,
        int top_k = 40,
        float presence_penalty = 0.0f,
        float frequency_penalty = 0.0f) {
        return {
            {"type", CommandType::EXECUTE},
            {"event_id", event_id},
            {"messages", messages},
            {"tools", tools},
            {"streaming", streaming},
            {"max_tokens", max_tokens},
            {"temperature", temperature},
            {"top_p", top_p},
            {"top_k", top_k},
            {"presence_penalty", presence_penalty},
            {"frequency_penalty", frequency_penalty}
        };
    }

    static json createResetCommand(const std::string& command_id = "") {
        json cmd = {{"type", CommandType::RESET}};
        if (!command_id.empty()) cmd["command_id"] = command_id;
        return cmd;
    }

    static json createSaveKvCommand(const std::string& checkpoint_name,
                                     const std::string& command_id = "") {
        json cmd = {
            {"type", CommandType::SAVE_KV},
            {"checkpoint_name", checkpoint_name}
        };
        if (!command_id.empty()) cmd["command_id"] = command_id;
        return cmd;
    }

    static json createRestoreKvCommand(const std::string& checkpoint_name,
                                        const std::string& command_id = "") {
        json cmd = {
            {"type", CommandType::RESTORE_KV},
            {"checkpoint_name", checkpoint_name}
        };
        if (!command_id.empty()) cmd["command_id"] = command_id;
        return cmd;
    }

    static json createShutdownCommand() {
        return {{"type", CommandType::SHUTDOWN}};
    }

    static json createClearSessionCommand(const std::string& session_id,
                                           const std::string& command_id = "") {
        json cmd = {
            {"type",       CommandType::CLEAR_SESSION},
            {"session_id", session_id}
        };
        if (!command_id.empty()) cmd["command_id"] = command_id;
        return cmd;
    }

    // ── Response parsers ──────────────────────────────────────────────────────

    static IPCReadyEvent parseReady(const json& msg) {
        return {
            msg.value("command_id", ""),
            msg.value("event_id", "")
        };
    }

    static IPCTokenEvent parseToken(const json& msg) {
        return {
            ipcGetStringOrDefault(msg, "event_id", ""),
            ipcGetStringOrDefault(msg, "content", "")
        };
    }

    static IPCDoneEvent parseDone(const json& msg) {
        return {
            msg.value("event_id", ""),
            msg.value("finish_reason", "stop")
        };
    }

    static IPCErrorEvent parseError(const json& msg) {
        return {
            msg.value("event_id", ""),
            msg.value("command_id", ""),
            msg.value("message", "Unknown error")
        };
    }
};
