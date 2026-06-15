// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// NativeToolRegistry.h — In-process tool registry
//
// Singleton that holds all ITool instances registered at startup.
// Called by McpNativeClient to list and execute native tools.
//
// Plug-and-play usage (in main.cpp):
//   auto& reg = NativeToolRegistry::getInstance();
//   reg.registerTool(std::make_unique<DateTimeTool>());
//   reg.registerTool(std::make_unique<CalculatorTool>());
//   // That's it — McpNativeClient picks these up automatically at connect().
// ─────────────────────────────────────────────────────────────────────────────

#include "mcp/ITool.h"
#include "mcp/McpTypes.h"
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class NativeToolRegistry {
public:
    static NativeToolRegistry& getInstance();

    void registerTool(std::unique_ptr<ITool> tool);
    std::vector<McpTool> listTools() const;
    bool hasTools() const;
    size_t toolCount() const;
    bool hasTool(const std::string& name) const;
    McpToolResult callTool(const std::string& name, const json& arguments);

private:
    NativeToolRegistry() = default;
    std::map<std::string, std::unique_ptr<ITool>> tools_;
    mutable std::mutex mutex_;
};
