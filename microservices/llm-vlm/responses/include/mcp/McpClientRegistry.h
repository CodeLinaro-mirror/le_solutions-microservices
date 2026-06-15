// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// McpClientRegistry.h — MCP server registry and tool aggregator
//
// Singleton that holds all McpClient instances. Initialized at startup from
// mcp_servers.json (or RESPONSES_MCP_CONFIG env var).
//
// Responsibilities:
//   1. Load server configs from JSON file
//   2. Create and connect McpClient instances (stdio or http)
//   3. Aggregate tool lists from all servers into a single OpenAI tools array
//   4. Route tool calls to the correct server by resolving the namespace prefix
//   5. Enforce allowed_tools filtering per request
//
// Tool name namespacing:
//   MCP tool "read_file" on server "filesystem" →
//   OpenAI function name "filesystem__read_file"
//
//   When the model calls "filesystem__read_file", the registry strips the
//   "filesystem__" prefix and routes to the "filesystem" server.
//
// Thread safety: all public methods are protected by a shared_mutex.
// Multiple concurrent reads (getAllTools, resolveServer) are allowed;
// writes (registerServer, connectAll) are exclusive.
// ─────────────────────────────────────────────────────────────────────────────

#include "mcp/McpClient.h"
#include "mcp/McpTypes.h"
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

class McpClientRegistry {
public:
    // ── Singleton ─────────────────────────────────────────────────────────────
    static McpClientRegistry& getInstance();

    // ── Configuration ─────────────────────────────────────────────────────────

    /**
     * Load server configurations from a JSON file and register them.
     * File format: {"mcp_servers": [{name, transport, command/url, ...}]}
     *
     * Called once at startup (in main.cpp registerBeginningAdvice).
     * If the file does not exist, logs a warning and returns (no error).
     *
     * @param config_path  Path to mcp_servers.json
     */
    void loadConfig(const std::string& config_path);

    /**
     * Register a single server configuration.
     * Does not connect — call connectAll() after all servers are registered.
     */
    void registerServer(const McpServerConfig& config);

    /**
     * Connect all registered servers (initialize handshake + tools/list).
     * Servers that fail to connect are logged and skipped — the registry
     * continues to function with the remaining servers.
     */
    void connectAll();

    /**
     * Disconnect all servers and clear the registry.
     * Called at shutdown.
     */
    void disconnectAll();

    // ── Tool Aggregation ──────────────────────────────────────────────────────

    /**
     * Return an OpenAI-format tools array aggregating all tools from all
     * connected servers. Tool names are namespaced as "{server}__{tool}".
     *
     * If allowed_tools is non-empty, only tools whose bare names appear in
     * allowed_tools are included (for the given server_label).
     *
     * @param server_label   If non-empty, only include tools from this server
     * @param allowed_tools  If non-empty, only include these bare tool names
     * @return               OpenAI function tools array
     */
    json getAllTools(const std::string& server_label = "",
                     const std::vector<std::string>& allowed_tools = {}) const;

    // ── Tool Routing ──────────────────────────────────────────────────────────

    /**
     * Invoke a tool by its namespaced name (e.g. "filesystem__read_file").
     *
     * Strips the server prefix, routes to the correct McpClient, and returns
     * the McpToolResult. Also records the call in the provided records vector.
     *
     * @param namespaced_name  Tool name as seen by the model (with __ prefix)
     * @param arguments        Tool arguments JSON object
     * @param call_id          Unique call ID for the McpCallRecord
     * @param records          Output: appended with the McpCallRecord
     * @return                 McpToolResult (content + isError)
     * @throws McpException    If server not found or call fails
     */
    McpToolResult callTool(const std::string& namespaced_name,
                            const json& arguments,
                            const std::string& call_id,
                            std::vector<McpCallRecord>& records);

    /**
     * Resolve which server owns a namespaced tool name.
     * Returns the server_label, or empty string if not found.
     */
    std::string resolveServer(const std::string& namespaced_name) const;

    /**
     * Returns true if at least one server is connected.
     */
    bool hasConnectedServers() const;

    /**
     * Returns the number of registered servers.
     */
    size_t serverCount() const;

    /**
     * Returns true if a server with the given label is registered.
     */
    bool hasServer(const std::string& server_label) const;

private:
    McpClientRegistry() = default;

    // Split "server__tool" → {"server", "tool"}
    // Returns {"", name} if no __ separator found.
    static std::pair<std::string, std::string> splitNamespacedName(
        const std::string& namespaced_name);

    // ── State ─────────────────────────────────────────────────────────────────
    mutable std::shared_mutex                          mutex_;
    std::map<std::string, std::unique_ptr<McpClient>>  clients_;   // label → client
    std::vector<McpServerConfig>                       configs_;   // registration order
};
