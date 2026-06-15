// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

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
