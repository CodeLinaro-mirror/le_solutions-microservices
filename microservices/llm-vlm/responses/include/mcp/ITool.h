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
//
// Tool name conventions:
//   - Lowercase snake_case: "datetime", "calculator", "web_search"
//   - No namespace prefix here — NativeToolRegistry / McpNativeClient add
//     the "native__" prefix when exposing to McpClientRegistry.
//
// Thread safety:
//   execute() may be called concurrently from multiple Drogon worker threads.
//   Implementations must be stateless or internally thread-safe.
// ─────────────────────────────────────────────────────────────────────────────

#include "mcp/McpTypes.h"
#include <string>

class ITool {
public:
    virtual ~ITool() = default;

    // ── Identity ──────────────────────────────────────────────────────────────

    /**
     * Bare tool name (no namespace prefix), e.g. "datetime".
     * Must be unique within the NativeToolRegistry.
     * Used as the function name when the model calls the tool.
     */
    virtual std::string name() const = 0;

    /**
     * Human-readable description shown to the model in its tool context.
     * Be specific about when to use this tool — the model uses this to decide
     * whether to call it. Include example trigger phrases.
     */
    virtual std::string description() const = 0;

    /**
     * JSON Schema for the tool's input arguments.
     * Passed directly to the model as the "parameters" field of the function
     * tool definition. Must be a valid JSON Schema object.
     *
     * For tools with no arguments, return:
     *   {"type": "object", "properties": {}, "required": []}
     */
    virtual json inputSchema() const = 0;

    // ── Execution ─────────────────────────────────────────────────────────────

    /**
     * Execute the tool with the given arguments.
     *
     * @param arguments  JSON object matching the inputSchema. May be empty
     *                   ({}) for tools with no required arguments.
     * @return           McpToolResult with content[] items. Set is_error=true
     *                   and include an error message in content[0].text if
     *                   execution fails — the model will receive the error
     *                   message and can respond accordingly.
     *
     * Thread safety: must be safe to call concurrently.
     */
    virtual McpToolResult execute(const json& arguments) = 0;

    // ── Helpers ───────────────────────────────────────────────────────────────

    /**
     * Build a successful McpToolResult with a single text content item.
     */
    static McpToolResult makeTextResult(const std::string& text) {
        McpToolResult result;
        result.is_error = false;
        McpContentItem item;
        item.type = "text";
        item.text = text;
        result.content.push_back(item);
        return result;
    }

    /**
     * Build an error McpToolResult with a descriptive message.
     */
    static McpToolResult makeErrorResult(const std::string& error_message) {
        McpToolResult result;
        result.is_error = true;
        McpContentItem item;
        item.type = "text";
        item.text = error_message;
        result.content.push_back(item);
        return result;
    }

    /**
     * Convert this tool to an McpTool descriptor (for McpNativeClient caching).
     */
    McpTool toMcpTool() const {
        McpTool t;
        t.name         = name();
        t.description  = description();
        t.input_schema = inputSchema();
        return t;
    }
};
