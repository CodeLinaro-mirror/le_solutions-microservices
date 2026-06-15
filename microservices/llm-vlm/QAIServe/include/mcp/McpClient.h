// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// McpClient.h — Abstract MCP client interface
//
// Each concrete subclass implements one transport:
//   McpStdioClient  — spawns MCP server as subprocess, JSON-RPC over stdio
//   McpHttpClient   — connects to remote MCP server over HTTP+SSE
//
// Usage pattern (same for both transports):
//
//   auto client = std::make_unique<McpStdioClient>(config);
//   client->connect();                    // handshake + tools/list
//   json tools = client->listTools();     // cached after connect()
//   json result = client->callTool("read_file", {{"path", "/etc/hosts"}});
//   client->disconnect();
//
// Thread safety: McpClient instances are NOT thread-safe. The McpClientRegistry
// serialises all calls with a per-client mutex.
// ─────────────────────────────────────────────────────────────────────────────

#include "mcp/McpTypes.h"
#include <memory>
#include <vector>

class McpClient {
public:
    virtual ~McpClient() = default;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    /**
     * Establish connection to the MCP server and perform the initialization
     * handshake (initialize → notifications/initialized → tools/list).
     *
     * After connect() returns, listTools() returns the cached tool list.
     *
     * @throws McpException on connection failure or protocol error
     */
    virtual void connect() = 0;

    /**
     * Gracefully disconnect from the MCP server.
     * For stdio: sends SIGTERM to the subprocess.
     * For HTTP: closes the connection.
     */
    virtual void disconnect() = 0;

    /**
     * Returns true if the connection is currently active.
     */
    virtual bool isConnected() const = 0;

    /**
     * Attempt to reconnect after a failure. Calls disconnect() then connect().
     * @throws McpException if reconnection fails
     */
    void reconnect() {
        disconnect();
        connect();
    }

    // ── MCP Protocol Methods ──────────────────────────────────────────────────

    /**
     * Return the cached tool list (populated during connect()).
     * Does NOT re-query the server.
     */
    virtual const std::vector<McpTool>& getCachedTools() const = 0;

    /**
     * Re-query the server for the current tool list and update the cache.
     * @throws McpException on protocol error
     */
    virtual std::vector<McpTool> listTools() = 0;

    /**
     * Invoke a tool on the MCP server.
     *
     * @param name       Tool name (bare, without server_label prefix)
     * @param arguments  Tool arguments as a JSON object
     * @return           McpToolResult with content[] and isError flag
     * @throws McpException on transport or protocol error
     */
    virtual McpToolResult callTool(const std::string& name, const json& arguments) = 0;

    // ── Identity ──────────────────────────────────────────────────────────────

    /**
     * Returns the logical server name (from McpServerConfig::name).
     */
    virtual std::string serverName() const = 0;

    /**
     * Returns the transport type: "stdio" or "http".
     */
    virtual std::string transportType() const = 0;

    // ── Static Helpers ────────────────────────────────────────────────────────

    /**
     * Translate an MCP tool descriptor to OpenAI function tool format.
     * Tool name is namespaced as "{server_label}__{tool_name}".
     *
     * Input (MCP tools/list item):
     *   {"name":"read_file","description":"...","inputSchema":{...}}
     *
     * Output (OpenAI function tool):
     *   {"type":"function","function":{"name":"filesystem__read_file",
     *    "description":"[filesystem] ...","parameters":{...}}}
     */
    static json mcpToolToOpenAI(const McpTool& tool, const std::string& server_label) {
        return tool.to_openai_function(server_label);
    }

    /**
     * Factory: create the appropriate McpClient subclass from a config.
     * Returns nullptr if transport is unknown.
     */
    static std::unique_ptr<McpClient> create(const McpServerConfig& config);
};
