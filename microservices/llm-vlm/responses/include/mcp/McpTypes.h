// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// McpTypes.h — Core MCP JSON-RPC 2.0 types
//
// Defines the data structures used throughout the MCP client layer:
//   - McpServerConfig   : configuration for one MCP server endpoint
//   - McpRequest        : outgoing JSON-RPC 2.0 request
//   - McpResponse       : incoming JSON-RPC 2.0 response
//   - McpError          : JSON-RPC error object
//   - McpTool           : a single tool descriptor from tools/list
//   - McpToolResult     : result from tools/call
//   - McpCallRecord     : audit record of one tool invocation (for output[])
//
// All wire serialisation uses nlohmann::json (already a project dependency).
// ─────────────────────────────────────────────────────────────────────────────

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <stdexcept>

using json = nlohmann::ordered_json;

// ─────────────────────────────────────────────────────────────────────────────
// McpServerConfig — one entry from mcp_servers.json
// ─────────────────────────────────────────────────────────────────────────────
struct McpServerConfig {
    std::string name;                              // Logical label, e.g. "filesystem"
    std::string transport;                         // "stdio" | "http"

    // stdio transport fields
    std::string command;                           // Executable path
    std::vector<std::string> args;                 // Command-line arguments
    std::map<std::string, std::string> env;        // Extra environment variables

    // http transport fields
    std::string url;                               // Base URL, e.g. "http://localhost:3001"

    static McpServerConfig from_json(const json& j) {
        McpServerConfig cfg;
        cfg.name      = j.at("name").get<std::string>();
        cfg.transport = j.value("transport", "stdio");
        cfg.command   = j.value("command", "");
        cfg.url       = j.value("url", "");
        if (j.contains("args") && j["args"].is_array()) {
            for (const auto& a : j["args"]) cfg.args.push_back(a.get<std::string>());
        }
        if (j.contains("env") && j["env"].is_object()) {
            for (auto& [k, v] : j["env"].items()) cfg.env[k] = v.get<std::string>();
        }
        return cfg;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// McpError — JSON-RPC 2.0 error object
// ─────────────────────────────────────────────────────────────────────────────
struct McpError {
    int         code    = 0;
    std::string message;
    json        data    = nullptr;

    static McpError from_json(const json& j) {
        McpError e;
        e.code    = j.value("code", 0);
        e.message = j.value("message", "");
        if (j.contains("data")) e.data = j["data"];
        return e;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// McpRequest — outgoing JSON-RPC 2.0 request
// ─────────────────────────────────────────────────────────────────────────────
struct McpRequest {
    int         id     = 0;
    std::string method;
    json        params = json::object();

    json to_json() const {
        json j = {
            {"jsonrpc", "2.0"},
            {"id",      id},
            {"method",  method},
            {"params",  params}
        };
        return j;
    }

    // Notification (no id)
    static McpRequest notification(const std::string& method, const json& params = json::object()) {
        McpRequest r;
        r.id     = -1;   // sentinel: no id
        r.method = method;
        r.params = params;
        return r;
    }

    json to_notification_json() const {
        return {{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// McpResponse — incoming JSON-RPC 2.0 response
// ─────────────────────────────────────────────────────────────────────────────
struct McpResponse {
    int                  id     = 0;
    json                 result = nullptr;
    std::optional<McpError> error;

    bool is_error() const { return error.has_value(); }

    static McpResponse from_json(const json& j) {
        McpResponse r;
        r.id = j.value("id", 0);
        if (j.contains("error") && !j["error"].is_null()) {
            r.error = McpError::from_json(j["error"]);
        } else if (j.contains("result")) {
            r.result = j["result"];
        }
        return r;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// McpTool — one tool descriptor from tools/list response
// ─────────────────────────────────────────────────────────────────────────────
struct McpTool {
    std::string name;
    std::string description;
    json        input_schema = json::object();   // JSON Schema for arguments

    static McpTool from_json(const json& j) {
        McpTool t;
        t.name        = j.value("name", "");
        t.description = j.value("description", "");
        if (j.contains("inputSchema")) t.input_schema = j["inputSchema"];
        return t;
    }

    // Convert to OpenAI function tool format, namespaced as
    // "{server_label}__{tool_name}" to avoid cross-server collisions.
    json to_openai_function(const std::string& server_label) const {
        std::string namespaced_name = server_label + "__" + name;
        return {
            {"type", "function"},
            {"function", {
                {"name",        namespaced_name},
                {"description", "[" + server_label + "] " + description},
                {"parameters",  input_schema}
            }}
        };
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// McpContentItem — one item in a tools/call result content array
// ─────────────────────────────────────────────────────────────────────────────
struct McpContentItem {
    std::string type;    // "text" | "image" | "resource"
    std::string text;    // For type=="text"
    std::string mime_type;
    std::string data;    // For type=="image" (base64)

    static McpContentItem from_json(const json& j) {
        McpContentItem c;
        c.type      = j.value("type", "text");
        c.text      = j.value("text", "");
        c.mime_type = j.value("mimeType", "");
        c.data      = j.value("data", "");
        return c;
    }

    // Flatten to a plain string for injection into the model context
    std::string to_string() const {
        if (type == "text") return text;
        if (type == "image") return "[image: " + mime_type + ", base64 data omitted]";
        return "[resource: " + mime_type + "]";
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// McpToolResult — result from tools/call
// ─────────────────────────────────────────────────────────────────────────────
struct McpToolResult {
    std::vector<McpContentItem> content;
    bool is_error = false;

    // Flatten all content items to a single string
    std::string to_string() const {
        std::string out;
        for (const auto& item : content) {
            if (!out.empty()) out += "\n";
            out += item.to_string();
        }
        return out;
    }

    static McpToolResult from_json(const json& j) {
        McpToolResult r;
        r.is_error = j.value("isError", false);
        if (j.contains("content") && j["content"].is_array()) {
            for (const auto& item : j["content"]) {
                r.content.push_back(McpContentItem::from_json(item));
            }
        }
        return r;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// McpCallRecord — audit record of one tool invocation
//
// Accumulated by McpAgenticLoop and included in the Responses API output[].
// ─────────────────────────────────────────────────────────────────────────────
struct McpCallRecord {
    std::string id;            // e.g. "mcpcall_001"
    std::string server_label;  // e.g. "filesystem"
    std::string tool_name;     // bare name (no namespace prefix)
    std::string arguments;     // JSON string of arguments
    std::string output;        // Flattened result string
    bool        is_error = false;
    std::string error_message;

    // Serialise to Responses API output[] item format
    json to_output_item() const {
        return {
            {"type",         "mcp_call"},
            {"id",           id},
            {"server_label", server_label},
            {"name",         tool_name},
            {"arguments",    arguments},
            {"output",       is_error ? json(nullptr) : json(output)},
            {"error",        is_error ? json(error_message) : json(nullptr)}
        };
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// McpException — thrown by McpClient on protocol or transport errors
// ─────────────────────────────────────────────────────────────────────────────
struct McpException : std::runtime_error {
    int         code;
    std::string server;

    McpException(const std::string& server, int code, const std::string& msg)
        : std::runtime_error(msg), code(code), server(server) {}
};
