//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#pragma once

#include <drogon/HttpController.h>
#include <nlohmann/json.hpp>
#include "store/VectorStoreManager.h"

namespace controllers {

using json = nlohmann::json;

/**
 * @brief Drogon HTTP Controller that implements standard Model Context Protocol (MCP) server.
 * Enables autonomous coding agents in McpAgenticLoop to perform semantic code searches
 * and re-indexing after making source code edits.
 */
class McpController : public drogon::HttpController<McpController> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(McpController::handle, "/mcp", drogon::Post);
    METHOD_LIST_END

    void handle(const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    json handleInitialize(const json& params, int32_t id);
    json handleToolsList(int32_t id);
    json handleToolsCall(const json& params, int32_t id);
    json handleNotification(const std::string& method);

    // Core tools requested by the Agent
    json toolFileSearch(const json& args);
    json toolIndexFile(const json& args);

    // Helpers to create standard JSON-RPC responses
    json mcpSuccess(int32_t id, const std::string& text);
    json mcpError(int32_t id, const std::string& text);
    json rpcError(int32_t id, int32_t code, const std::string& msg);

    store::VectorStoreManager& manager_ = store::VectorStoreManager::getInstance();
};

} // namespace controllers
