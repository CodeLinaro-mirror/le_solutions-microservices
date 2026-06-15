// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// ITool.h — Abstract interface for a native (in-process) tool
//
// Every built-in tool implements this interface. The NativeToolRegistry holds
// a map of ITool instances and dispatches callTool() to the correct one.
//
// Plug-and-play pattern:
//   1. Create MyTool.h / MyTool.cpp implementing ITool
//   2. In main.cpp: native_registry.registerTool(std::make_unique<MyTool>());
//   3. Done — the model automatically sees the new tool on the next request.
// ─────────────────────────────────────────────────────────────────────────────

#include "mcp/McpTypes.h"
#include <string>

class ITool {
public:
    virtual ~ITool() = default;

    virtual std::string name() const = 0;
    virtual std::string description() const = 0;
    virtual json inputSchema() const = 0;
    virtual McpToolResult execute(const json& arguments) = 0;

    static McpToolResult makeTextResult(const std::string& text) {
        McpToolResult result;
        result.is_error = false;
        McpContentItem item;
        item.type = "text";
        item.text = text;
        result.content.push_back(item);
        return result;
    }

    static McpToolResult makeErrorResult(const std::string& error_message) {
        McpToolResult result;
        result.is_error = true;
        McpContentItem item;
        item.type = "text";
        item.text = error_message;
        result.content.push_back(item);
        return result;
    }

    McpTool toMcpTool() const {
        McpTool t;
        t.name         = name();
        t.description  = description();
        t.input_schema = inputSchema();
        return t;
    }
};
