// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// McpClientRegistry.cpp — MCP server registry and tool aggregator
//
// Singleton that manages all McpClient instances. Loaded from mcp_servers.json
// at startup. Provides tool aggregation and call routing.
// ─────────────────────────────────────────────────────────────────────────────

#include "mcp/McpClientRegistry.h"
#include "mcp/McpStdioClient.h"
#include "mcp/McpHttpClient.h"
#include "mcp/McpNativeClient.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// Singleton
// ─────────────────────────────────────────────────────────────────────────────
McpClientRegistry& McpClientRegistry::getInstance() {
    static McpClientRegistry instance;
    return instance;
}

// ─────────────────────────────────────────────────────────────────────────────
// McpClient::create — factory method (defined here to avoid circular deps)
// ─────────────────────────────────────────────────────────────────────────────
std::unique_ptr<McpClient> McpClient::create(const McpServerConfig& config) {
    if (config.transport == "stdio") {
        return std::make_unique<McpStdioClient>(config);
    }
    if (config.transport == "http" || config.transport == "https") {
        return std::make_unique<McpHttpClient>(config);
    }
    // "native" transport: in-process tools via NativeToolRegistry.
    if (config.transport == "native") {
        return std::make_unique<McpNativeClient>(config);
    }
    std::cerr << "[McpClientRegistry] Unknown transport '" << config.transport
              << "' for server '" << config.name << "'\n";
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// loadConfig — parse mcp_servers.json
// ─────────────────────────────────────────────────────────────────────────────
void McpClientRegistry::loadConfig(const std::string& config_path) {
    std::ifstream f(config_path);
    if (!f.is_open()) {
        std::cout << "[McpClientRegistry] Config file not found: " << config_path
                  << " — MCP support disabled\n";
        return;
    }

    json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        std::cerr << "[McpClientRegistry] Failed to parse " << config_path
                  << ": " << e.what() << "\n";
        return;
    }

    if (!j.contains("mcp_servers") || !j["mcp_servers"].is_array()) {
        std::cerr << "[McpClientRegistry] No 'mcp_servers' array in " << config_path << "\n";
        return;
    }

    for (const auto& server_json : j["mcp_servers"]) {
        try {
            McpServerConfig cfg = McpServerConfig::from_json(server_json);
            registerServer(cfg);
        } catch (const std::exception& e) {
            std::cerr << "[McpClientRegistry] Failed to parse server config: "
                      << e.what() << "\n";
        }
    }

    std::cout << "[McpClientRegistry] Loaded " << configs_.size()
              << " server config(s) from " << config_path << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// registerServer
// ─────────────────────────────────────────────────────────────────────────────
void McpClientRegistry::registerServer(const McpServerConfig& config) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    configs_.push_back(config);
    std::cout << "[McpClientRegistry] Registered server '" << config.name
              << "' (" << config.transport << ")\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// connectAll — create and connect all registered servers
// ─────────────────────────────────────────────────────────────────────────────
void McpClientRegistry::connectAll() {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    for (const auto& cfg : configs_) {
        if (clients_.count(cfg.name)) continue;  // already connected

        auto client = McpClient::create(cfg);
        if (!client) continue;

        try {
            client->connect();
            clients_[cfg.name] = std::move(client);
            std::cout << "[McpClientRegistry] Server '" << cfg.name << "' connected\n";
        } catch (const McpException& e) {
            std::cerr << "[McpClientRegistry] Failed to connect to '" << cfg.name
                      << "': " << e.what() << " — skipping\n";
        } catch (const std::exception& e) {
            std::cerr << "[McpClientRegistry] Failed to connect to '" << cfg.name
                      << "': " << e.what() << " — skipping\n";
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// disconnectAll
// ─────────────────────────────────────────────────────────────────────────────
void McpClientRegistry::disconnectAll() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    for (auto& [name, client] : clients_) {
        try {
            client->disconnect();
        } catch (...) {}
    }
    clients_.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// getAllTools — aggregate OpenAI-format tools from all connected servers
// ─────────────────────────────────────────────────────────────────────────────
json McpClientRegistry::getAllTools(const std::string& server_label,
                                     const std::vector<std::string>& allowed_tools) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    json tools = json::array();

    for (const auto& [name, client] : clients_) {
        // Filter by server_label if specified
        if (!server_label.empty() && name != server_label) continue;
        if (!client->isConnected()) continue;

        for (const auto& tool : client->getCachedTools()) {
            // Filter by allowed_tools if specified
            if (!allowed_tools.empty()) {
                bool allowed = std::find(allowed_tools.begin(),
                                         allowed_tools.end(),
                                         tool.name) != allowed_tools.end();
                if (!allowed) continue;
            }
            tools.push_back(tool.to_openai_function(name));
        }
    }

    return tools;
}

// ─────────────────────────────────────────────────────────────────────────────
// callTool — route namespaced tool call to the correct server
// ─────────────────────────────────────────────────────────────────────────────
McpToolResult McpClientRegistry::callTool(const std::string& namespaced_name,
                                            const json& arguments,
                                            const std::string& call_id,
                                            std::vector<McpCallRecord>& records) {
    auto [server_label, bare_name] = splitNamespacedName(namespaced_name);

    // Build the call record (will be updated with result)
    McpCallRecord record;
    record.id           = call_id;
    record.server_label = server_label;
    record.tool_name    = bare_name;
    record.arguments    = arguments.dump();

    // Find the client
    McpClient* client = nullptr;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = clients_.find(server_label);
        if (it == clients_.end()) {
            record.is_error      = true;
            record.error_message = "MCP server '" + server_label + "' not found in registry";
            records.push_back(record);
            McpToolResult err_result;
            err_result.is_error = true;
            McpContentItem item;
            item.type = "text";
            item.text = record.error_message;
            err_result.content.push_back(item);
            return err_result;
        }
        client = it->second.get();
    }

    // Execute the tool call
    McpToolResult result;
    try {
        result = client->callTool(bare_name, arguments);
    } catch (const McpException& e) {
        result.is_error = true;
        McpContentItem item;
        item.type = "text";
        item.text = std::string("MCP call failed: ") + e.what();
        result.content.push_back(item);
    } catch (const std::exception& e) {
        result.is_error = true;
        McpContentItem item;
        item.type = "text";
        item.text = std::string("Tool call error: ") + e.what();
        result.content.push_back(item);
    }

    // Update and store the record
    record.output        = result.to_string();
    record.is_error      = result.is_error;
    record.error_message = result.is_error ? result.to_string() : "";
    records.push_back(record);

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveServer
// ─────────────────────────────────────────────────────────────────────────────
std::string McpClientRegistry::resolveServer(const std::string& namespaced_name) const {
    auto [server_label, bare_name] = splitNamespacedName(namespaced_name);
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (clients_.count(server_label)) return server_label;
    return "";
}

// ─────────────────────────────────────────────────────────────────────────────
// hasConnectedServers / serverCount / hasServer
// ─────────────────────────────────────────────────────────────────────────────
bool McpClientRegistry::hasConnectedServers() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (const auto& [name, client] : clients_) {
        if (client->isConnected()) return true;
    }
    return false;
}

size_t McpClientRegistry::serverCount() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return configs_.size();
}

bool McpClientRegistry::hasServer(const std::string& server_label) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return clients_.count(server_label) > 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// splitNamespacedName — "server__tool" → {"server", "tool"}
// ─────────────────────────────────────────────────────────────────────────────
std::pair<std::string, std::string>
McpClientRegistry::splitNamespacedName(const std::string& namespaced_name) {
    auto pos = namespaced_name.find("__");
    if (pos == std::string::npos) {
        return {"", namespaced_name};
    }
    return {namespaced_name.substr(0, pos),
            namespaced_name.substr(pos + 2)};
}
