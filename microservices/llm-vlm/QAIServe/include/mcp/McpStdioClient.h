// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// McpStdioClient.h — stdio/subprocess MCP transport
//
// Spawns the MCP server as a child process and communicates via stdin/stdout
// using newline-delimited JSON-RPC 2.0 (one JSON object per line).
//
// Process lifecycle mirrors InferenceWorkerManager:
//   connect()    → pipe() + fork() + exec() → initialize handshake → tools/list
//   callTool()   → write JSON-RPC request to stdin → read response from stdout
//   disconnect() → SIGTERM to child process → waitpid()
//
// The child process inherits the parent's environment plus any extra env vars
// specified in McpServerConfig::env.
// ─────────────────────────────────────────────────────────────────────────────

#include "mcp/McpClient.h"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

class McpStdioClient : public McpClient {
public:
    explicit McpStdioClient(const McpServerConfig& config);
    ~McpStdioClient() override;

    // ── McpClient interface ───────────────────────────────────────────────────
    void connect() override;
    void disconnect() override;
    bool isConnected() const override;

    const std::vector<McpTool>& getCachedTools() const override;
    std::vector<McpTool> listTools() override;
    McpToolResult callTool(const std::string& name, const json& arguments) override;

    std::string serverName() const override   { return config_.name; }
    std::string transportType() const override { return "stdio"; }

private:
    // ── Internal helpers ──────────────────────────────────────────────────────

    // Send a JSON-RPC request and wait for the matching response (by id).
    // Blocks until the response arrives or timeout_ms elapses.
    McpResponse sendRequest(McpRequest& req, int timeout_ms = 30000);

    // Send a JSON-RPC notification (no response expected).
    void sendNotification(const McpRequest& notif);

    // Write one line to the child's stdin.
    void writeLine(const std::string& line);

    // Read one line from the child's stdout (blocks).
    std::string readLine(int timeout_ms = 30000);

    // Perform the MCP initialization handshake.
    void doHandshake();

    // Spawn the child process and set up pipes.
    void spawnProcess();

    // Kill and reap the child process.
    void killProcess();

    // ── State ─────────────────────────────────────────────────────────────────
    McpServerConfig      config_;
    std::vector<McpTool> cached_tools_;
    std::atomic<bool>    connected_{false};
    mutable std::mutex   mutex_;

    // File descriptors for the pipes to/from the child process
    int stdin_write_fd_  = -1;   // Parent writes → child stdin
    int stdout_read_fd_  = -1;   // Parent reads  ← child stdout
    int child_pid_       = -1;

    // JSON-RPC request ID counter
    int next_id_ = 1;
};
