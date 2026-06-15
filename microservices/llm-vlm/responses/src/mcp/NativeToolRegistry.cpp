// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "mcp/NativeToolRegistry.h"
#include <iostream>

NativeToolRegistry& NativeToolRegistry::getInstance() {
    static NativeToolRegistry instance;
    return instance;
}

void NativeToolRegistry::registerTool(std::unique_ptr<ITool> tool) {
    if (!tool) {
        std::cerr << "[NativeToolRegistry] registerTool() called with null tool\n";
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string tool_name = tool->name();
    if (tools_.count(tool_name)) {
        std::cerr << "[NativeToolRegistry] WARNING: overwriting existing tool '"
                  << tool_name << "'\n";
    }
    std::cout << "[NativeToolRegistry] Registered tool '" << tool_name << "'\n";
    tools_[tool_name] = std::move(tool);
}

std::vector<McpTool> NativeToolRegistry::listTools() const {
    std::vector<McpTool> result;
    result.reserve(tools_.size());
    for (const auto& [name, tool] : tools_) {
        result.push_back(tool->toMcpTool());
    }
    return result;
}

bool NativeToolRegistry::hasTools() const {
    return !tools_.empty();
}

size_t NativeToolRegistry::toolCount() const {
    return tools_.size();
}

bool NativeToolRegistry::hasTool(const std::string& name) const {
    return tools_.count(name) > 0;
}

McpToolResult NativeToolRegistry::callTool(const std::string& name,
                                            const json& arguments) {
    auto it = tools_.find(name);
    if (it == tools_.end()) {
        std::cerr << "[NativeToolRegistry] Unknown tool '" << name << "'\n";
        return ITool::makeErrorResult(
            "Native tool '" + name + "' is not registered. "
            "Available tools: " + [this]() {
                std::string list;
                for (const auto& [n, _] : tools_) {
                    if (!list.empty()) list += ", ";
                    list += n;
                }
                return list.empty() ? "(none)" : list;
            }()
        );
    }
    try {
        return it->second->execute(arguments);
    } catch (const std::exception& e) {
        std::cerr << "[NativeToolRegistry] Tool '" << name
                  << "' threw exception: " << e.what() << "\n";
        return ITool::makeErrorResult(std::string("Tool execution error: ") + e.what());
    } catch (...) {
        return ITool::makeErrorResult("Tool execution failed with unknown error");
    }
}
