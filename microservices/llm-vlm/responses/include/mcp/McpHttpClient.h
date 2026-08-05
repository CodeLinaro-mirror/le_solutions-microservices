// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// McpHttpClient.h — HTTP+SSE MCP transport
//
// Connects to a remote MCP server over HTTP. Implements the MCP 2024-11-05
// HTTP transport spec:
//   - POST {url}/mcp  with JSON-RPC request body
//   - Response is either a direct JSON-RPC response (for simple calls)
//     or an SSE stream (for streaming results)
//
// The initialization handshake and tools/list are performed at connect() time.
// Subsequent callTool() calls are synchronous POST requests.
//
// HTTP client: uses POSIX sockets directly (no external HTTP library required)
// to keep the dependency footprint minimal. For production use, replace with
// libcurl or Drogon's built-in HTTP client.
// ─────────────────────────────────────────────────────────────────────────────

#include "mcp/McpClient.h"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

class McpHttpClient : public McpClient {
public:
    explicit McpHttpClient(const McpServerConfig& config);
    ~McpHttpClient() override;

    // ── McpClient interface ───────────────────────────────────────────────────
    void connect() override;
    void disconnect() override;
    bool isConnected() const override;

    const std::vector<McpTool>& getCachedTools() const override;
    std::vector<McpTool> listTools() override;
    McpToolResult callTool(const std::string& name, const json& arguments) override;

    std::string serverName() const override    { return config_.name; }
    std::string transportType() const override  { return "http"; }

private:
    // ── Internal helpers ──────────────────────────────────────────────────────

    // Send a JSON-RPC request via HTTP POST and return the parsed response.
    McpResponse sendHttpRequest(McpRequest& req, int timeout_ms = 30000);

    // Perform the MCP initialization handshake.
    void doHandshake();

    // Low-level HTTP POST: sends body to {host}:{port}{path}, returns response body.
    std::string httpPost(const std::string& path,
                         const std::string& body,
                         int timeout_ms = 30000);

    // Parse host, port, path from the configured URL.
    void parseUrl(const std::string& url,
                  std::string& host,
                  int& port,
                  std::string& path) const;

    // ── State ─────────────────────────────────────────────────────────────────
    McpServerConfig      config_;
    std::vector<McpTool> cached_tools_;
    std::atomic<bool>    connected_{false};
    mutable std::mutex   mutex_;

    // Parsed URL components (set in connect())
    std::string host_;
    int         port_  = 80;
    std::string path_;   // e.g. "/mcp"

    // JSON-RPC request ID counter
    int next_id_ = 1;
};
