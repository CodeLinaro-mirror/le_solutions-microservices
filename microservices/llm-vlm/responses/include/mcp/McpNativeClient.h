// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// McpNativeClient.h — McpClient adapter for in-process native tools
//
// Implements the McpClient interface but executes tools in-process via
// NativeToolRegistry instead of spawning a subprocess or making HTTP calls.
//
// Transport type: "native"
// Server label:   "native" (registered in McpClientRegistry)
// ─────────────────────────────────────────────────────────────────────────────

#include "mcp/McpClient.h"
#include "mcp/NativeToolRegistry.h"
#include <atomic>
#include <string>
#include <vector>

class McpNativeClient : public McpClient {
public:
    explicit McpNativeClient(const McpServerConfig& config);
    ~McpNativeClient() override = default;

    void connect() override;
    void disconnect() override;
    bool isConnected() const override;
    const std::vector<McpTool>& getCachedTools() const override;
    std::vector<McpTool> listTools() override;
    McpToolResult callTool(const std::string& name, const json& arguments) override;

    std::string serverName() const override    { return config_.name; }
    std::string transportType() const override  { return "native"; }

private:
    McpServerConfig      config_;
    std::vector<McpTool> cached_tools_;
    std::atomic<bool>    connected_{false};
};
