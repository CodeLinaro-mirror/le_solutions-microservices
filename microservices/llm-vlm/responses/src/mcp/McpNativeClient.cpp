// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "mcp/McpNativeClient.h"
#include <iostream>

McpNativeClient::McpNativeClient(const McpServerConfig& config)
    : config_(config) {}

void McpNativeClient::connect() {
    if (connected_.load()) return;
    auto& registry = NativeToolRegistry::getInstance();
    cached_tools_ = registry.listTools();
    connected_.store(true);
    std::cout << "[McpNativeClient] Connected — "
              << cached_tools_.size() << " native tool(s) available: ";
    for (size_t i = 0; i < cached_tools_.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << cached_tools_[i].name;
    }
    std::cout << "\n";
}

void McpNativeClient::disconnect() {
    connected_.store(false);
    cached_tools_.clear();
}

bool McpNativeClient::isConnected() const {
    return connected_.load();
}

const std::vector<McpTool>& McpNativeClient::getCachedTools() const {
    return cached_tools_;
}

std::vector<McpTool> McpNativeClient::listTools() {
    auto& registry = NativeToolRegistry::getInstance();
    cached_tools_ = registry.listTools();
    return cached_tools_;
}

McpToolResult McpNativeClient::callTool(const std::string& name,
                                         const json& arguments) {
    if (!connected_.load()) {
        return ITool::makeErrorResult(
            "Native tool client is not connected. "
            "Ensure NativeToolRegistry is initialized before calling tools.");
    }
    auto& registry = NativeToolRegistry::getInstance();
    return registry.callTool(name, arguments);
}
