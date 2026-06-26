//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#include "controllers/McpController.h"
#include <fstream>
#include <regex>
#include <iostream>

namespace controllers {

void McpController::handle(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback
) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);

    json body;
    try {
        body = json::parse(req->body());
    } catch (const std::exception& e) {
        resp->setBody(rpcError(0, -32700, "Parse error: Invalid JSON received.").dump());
        callback(resp);
        return;
    }

    std::string method = body.value("method", "");
    int32_t id = body.value("id", 0);

    json response_json;
    if (method == "initialize") {
        response_json = handleInitialize(body.value("params", json::object()), id);
    } else if (method == "tools/list") {
        response_json = handleToolsList(id);
    } else if (method == "tools/call") {
        response_json = handleToolsCall(body.value("params", json::object()), id);
    } else if (method == "notifications/initialized") {
        response_json = handleNotification(method);
    } else {
        response_json = rpcError(id, -32601, "Method not found: " + method);
    }

    resp->setBody(response_json.dump());
    callback(resp);
}

json McpController::handleInitialize(const json& params, int32_t id) {
    return json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", {
            {"protocolVersion", "2024-11-05"},
            {"capabilities", {
                {"tools", json::object()}
            }},
            {"serverInfo", {
                {"name", "vector-store-mcp-server"},
                {"version", "1.0"}
            }}
        }}
    };
}

json McpController::handleNotification(const std::string& method) {
    // Standard MCP/JSON-RPC notification returns an empty JSON object/HTTP 200
    return json::object();
}

json McpController::handleToolsList(int32_t id) {
    json tools = json::array();

    // 1. file_search
    json file_search = {
        {"name", "file_search"},
        {"description", "Search the codebase for files relevant to a query"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"query", {{"type", "string"}, {"description", "Natural language description of what you are looking for"}}},
                {"vector_store_id", {{"type", "string"}, {"description", "ID of the vector store to search (e.g. 'vs_codebase')"}}},
                {"top_k", {{"type", "integer"}, {"description", "Number of results to return"}, {"default", 5}}},
                {"score_threshold", {{"type", "number"}, {"description", "Minimum cosine similarity score (0.0-1.0). Recommended: 0.65 for code search"}, {"default", 0.65}}}
            }},
            {"required", json::array({"query", "vector_store_id"})}
        }}
    };

    // 2. index_file
    json index_file = {
        {"name", "index_file"},
        {"description", "Re-index a file after it has been edited"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"file_path", {{"type", "string"}, {"description", "Absolute or relative path to the file to re-index"}}},
                {"vector_store_id", {{"type", "string"}, {"description", "ID of the vector store to update"}}}
            }},
            {"required", json::array({"file_path", "vector_store_id"})}
        }}
    };

    tools.push_back(file_search);
    tools.push_back(index_file);

    return json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", {
            {"tools", tools}
        }}
    };
}

json McpController::handleToolsCall(const json& params, int32_t id) {
    std::string tool_name = params.value("name", "");
    json args = params.value("arguments", json::object());

    if (tool_name == "file_search") {
        return mcpSuccess(id, toolFileSearch(args).value("text", ""));
    } else if (tool_name == "index_file") {
        return mcpSuccess(id, toolIndexFile(args).value("text", ""));
    } else {
        return rpcError(id, -32602, "Invalid tool name: " + tool_name);
    }
}

json McpController::toolFileSearch(const json& args) {
    try {
        std::string query = args.at("query").get<std::string>();
        std::string store_id = args.at("vector_store_id").get<std::string>();
        int32_t top_k = args.value("top_k", 5);
        float threshold = args.value("score_threshold", 0.65f);

        store::SearchRequest s_req;
        s_req.query = query;
        s_req.top_k = top_k;
        s_req.score_threshold = threshold;

        auto s_res = manager_.searchStore(store_id, s_req);

        std::stringstream ss;
        if (s_res.results.empty()) {
            ss << "No results found matching threshold (" << threshold << ").";
        } else {
            ss << "Found " << s_res.results.size() << " relevant files:\n\n";
            for (size_t i = 0; i < s_res.results.size(); ++i) {
                const auto& item = s_res.results[i];
                ss << i + 1 << ". " << item.file_id << " (score: " << item.score << ")\n";
                if (item.metadata.contains("function_name")) {
                    ss << "   function: " << item.metadata["function_name"].get<std::string>();
                    if (item.metadata.contains("line_start")) {
                        ss << " — lines " << item.metadata["line_start"] << "-" << item.metadata["line_end"];
                    }
                    ss << "\n";
                }
                ss << "   excerpt: \"" << item.text << "\"\n\n";
            }
        }

        return json{{"text", ss.str()}};

    } catch (const std::exception& e) {
        return json{{"text", std::string("Error during search: ") + e.what()}};
    }
}

json McpController::toolIndexFile(const json& args) {
    try {
        std::string file_path = args.at("file_path").get<std::string>();
        std::string store_id = args.at("vector_store_id").get<std::string>();

        // 1. Read file content
        std::ifstream file(file_path);
        if (!file.is_open()) {
            return json{{"text", "Error: Could not open or find file: " + file_path}};
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string file_content = buffer.str();
        file.close();

        // 2. Perform Code-Aware Function-Level Chunking (P1 Requirement)
        // Split file content into function-level blocks using simple heuristics
        std::vector<store::DocumentChunk> chunks;
        std::vector<std::string> lines;
        std::string line;
        std::stringstream content_stream(file_content);
        while (std::getline(content_stream, line)) {
            lines.push_back(line);
        }

        // Search for C++ style function bounds, e.g. "T Class::func(...) {" or "T func(...) {"
        std::regex func_sig(R"(^([a-zA-Z_]\w*\s+)*[a-zA-Z_]\w*(::[a-zA-Z_]\w*)?\(.*\)\s*\{?$)");

        int32_t current_func_start = -1;
        std::string current_func_name = "";
        std::stringstream current_func_body;

        for (size_t idx = 0; idx < lines.size(); ++idx) {
            std::string trimmed_line = lines[idx];
            // Strip leading spaces
            trimmed_line.erase(0, trimmed_line.find_first_not_of(" \t"));

            std::smatch match;
            if (std::regex_match(trimmed_line, match, func_sig)) {
                // If there's an ongoing function, save it
                if (current_func_start != -1) {
                    store::DocumentChunk chunk;
                    chunk.text = current_func_body.str();
                    chunk.metadata = {
                        {"file_path", file_path},
                        {"function_name", current_func_name},
                        {"line_start", current_func_start + 1},
                        {"line_end", static_cast<int32_t>(idx)}
                    };
                    chunks.push_back(chunk);
                    current_func_body.str("");
                    current_func_body.clear();
                }
                current_func_start = idx;
                current_func_name = trimmed_line.substr(0, trimmed_line.find('('));
            }

            if (current_func_start != -1) {
                current_func_body << lines[idx] << "\n";
            }
        }

        // Save last function
        if (current_func_start != -1) {
            store::DocumentChunk chunk;
            chunk.text = current_func_body.str();
            chunk.metadata = {
                {"file_path", file_path},
                {"function_name", current_func_name},
                {"line_start", current_func_start + 1},
                {"line_end", static_cast<int32_t>(lines.size())}
            };
            chunks.push_back(chunk);
        }

        // If no functions identified, fall back to simple line-count grouping
        if (chunks.empty()) {
            std::stringstream fallback_chunk;
            for (size_t idx = 0; idx < lines.size(); ++idx) {
                fallback_chunk << lines[idx] << "\n";
                if ((idx > 0 && idx % 30 == 0) || idx == lines.size() - 1) {
                    store::DocumentChunk chunk;
                    chunk.text = fallback_chunk.str();
                    chunk.metadata = {
                        {"file_path", file_path},
                        {"line_start", static_cast<int32_t>(idx - fallback_chunk.str().length() / 80)},
                        {"line_end", static_cast<int32_t>(idx + 1)}
                    };
                    chunks.push_back(chunk);
                    fallback_chunk.str("");
                    fallback_chunk.clear();
                }
            }
        }

        // 3. Delete existing file indices to ensure we avoid duplicates (Soft Delete)
        manager_.repository().deleteFile(store_id, file_path);

        // 4. Ingest new chunks (This uses the async background thread pool)
        std::string job_id = manager_.enqueueIngestion(store_id, file_path, std::move(chunks));

        std::string out = "Re-indexing triggered for " + file_path + ". Registered background job " + job_id + " to process " + std::to_string(chunks.size()) + " code chunks.";
        return json{{"text", out}};

    } catch (const std::exception& e) {
        return json{{"text", std::string("Error during re-indexing: ") + e.what()}};
    }
}

json McpController::mcpSuccess(int32_t id, const std::string& text) {
    return json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", {
            {"content", json::array({
                {{"type", "text"}, {"text", text}}
            })},
            {"isError", false}
        }}
    };
}

json McpController::mcpError(int32_t id, const std::string& text) {
    return json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", {
            {"content", json::array({
                {{"type", "text"}, {"text", text}}
            })},
            {"isError", true}
        }}
    };
}

json McpController::rpcError(int32_t id, int32_t code, const std::string& msg) {
    return json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", {
            {"code", code},
            {"message", msg}
        }}
    };
}

} // namespace controllers
