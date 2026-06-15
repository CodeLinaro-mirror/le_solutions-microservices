// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/adapters/Qwen3Adapter.h"
#include "qai_forge/utils/Logger.h"
#include <sstream>
#include <regex>

// ─────────────────────────────────────────────────────────────────────────────
// Qwen3Adapter — Vision Preprocessing
//
// Qwen 3-VL uses a simpler <|image|> token format.
// ─────────────────────────────────────────────────────────────────────────────
json Qwen3Adapter::preprocessVision(const json& messages) const {
    json processed = json::array();

    for (const auto& msg : messages) {
        if (!msg.is_object()) { processed.push_back(msg); continue; }

        std::string role = msg.value("role", "");
        const auto& content = msg["content"];

        if (content.is_string()) {
            processed.push_back(msg);
            continue;
        }

        if (content.is_array()) {
            std::string text_content;
            std::vector<std::string> image_urls;

            for (const auto& part : content) {
                std::string type = part.value("type", "");
                if (type == "text") {
                    text_content += part.value("text", "");
                } else if (type == "image_url") {
                    auto image_url = part.value("image_url", json::object());
                    std::string url = image_url.value("url", "");
                    if (!url.empty()) {
                        image_urls.push_back(url);
                        // Qwen 3-VL uses <|image|> token (different from Qwen 2.5)
                        text_content += "<|image|>";
                    }
                }
            }

            json new_msg = {
                {"role", role},
                {"content", text_content}
            };
            if (!image_urls.empty()) {
                new_msg["_image_urls"] = image_urls;
            }
            processed.push_back(new_msg);
        } else {
            processed.push_back(msg);
        }
    }

    return processed;
}

// ─────────────────────────────────────────────────────────────────────────────
// Qwen3Adapter — Tool Instructions
//
// Qwen 3 uses a native tool call format with a different system prompt structure.
// ─────────────────────────────────────────────────────────────────────────────
std::string Qwen3Adapter::formatToolInstructions(const json& tools) const {
    if (tools.empty() || !tools.is_array()) return "";

    std::ostringstream oss;
    oss << "\n\n# Tools\n\n";
    oss << "You have access to the following tools. Use them when appropriate.\n\n";

    for (const auto& tool : tools) {
        if (!tool.is_object()) continue;
        std::string type = tool.value("type", "");
        if (type != "function") continue;

        auto func = tool.value("function", json::object());
        std::string name = func.value("name", "");
        std::string description = func.value("description", "");
        json parameters = func.value("parameters", json::object());

        oss << "## " << name << "\n";
        oss << description << "\n\n";
        oss << "Parameters:\n" << parameters.dump(2) << "\n\n";
    }

    oss << "When you need to call a tool, respond with:\n";
    oss << "<tool_call>\n";
    oss << "{\"name\": \"<tool_name>\", \"arguments\": {\"param\": \"value\"}}\n";
    oss << "</tool_call>\n";
    oss << "You can call multiple tools in sequence if needed.";

    return oss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// Qwen3Adapter — Parse Tool Calls
//
// Same <tool_call> tag format as Qwen 2.5 but different JSON structure.
// ─────────────────────────────────────────────────────────────────────────────
json Qwen3Adapter::parseToolCalls(const std::string& response_text) const {
    json tool_calls = json::array();

    static const std::regex tool_call_re(
        R"(<tool_call>\s*([\s\S]*?)\s*</tool_call>)",
        std::regex::ECMAScript
    );

    auto begin = std::sregex_iterator(response_text.begin(), response_text.end(), tool_call_re);
    auto end = std::sregex_iterator();

    int idx = 0;
    for (auto it = begin; it != end; ++it, ++idx) {
        std::string call_json = (*it)[1].str();
        try {
            json call = json::parse(call_json);
            std::string name = call.value("name", "");
            // Qwen 3 uses "arguments" as a nested object (same key, different nesting)
            json arguments = call.value("arguments", json::object());

            std::string args_str = arguments.is_string()
                ? arguments.get<std::string>()
                : arguments.dump();

            tool_calls.push_back({
                {"id", "call_" + std::to_string(idx)},
                {"type", "function"},
                {"function", {
                    {"name", name},
                    {"arguments", args_str}
                }}
            });
        } catch (const std::exception& e) {
            LOG_WARN("[Qwen3Adapter] Failed to parse tool_call: " << e.what());
        }
    }

    return tool_calls;
}

// ─────────────────────────────────────────────────────────────────────────────
// Qwen3Adapter — Format Tool Response
// ─────────────────────────────────────────────────────────────────────────────
std::string Qwen3Adapter::formatToolResponse(const json& tool_results) const {
    std::ostringstream oss;
    for (const auto& result : tool_results) {
        if (!result.is_object()) continue;
        std::string tool_call_id = result.value("tool_call_id", "");
        std::string content = result.value("content", "");
        oss << "<tool_response>\n";
        oss << "{\"tool_call_id\": \"" << tool_call_id << "\", ";
        oss << "\"result\": " << json(content).dump() << "}\n";
        oss << "</tool_response>\n";
    }
    return oss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// Qwen3Adapter — Build System Prompt
//
// Qwen 3 supports thinking mode. The system prompt can include a thinking
// budget hint when the model is in reasoning mode.
// ─────────────────────────────────────────────────────────────────────────────
std::string Qwen3Adapter::buildSystemPrompt(const json& chat_template,
                                             const std::string& user_system,
                                             const json& tools) const {
    std::string default_system = chat_template.value("default_system_prompt",
        "You are Qwen, created by Alibaba Cloud. You are a helpful assistant.");

    std::string system = user_system.empty() ? default_system : user_system;

    // Inject tool instructions if tools are provided
    if (!tools.empty() && tools.is_array()) {
        system += formatToolInstructions(tools);
    }

    return system;
}
