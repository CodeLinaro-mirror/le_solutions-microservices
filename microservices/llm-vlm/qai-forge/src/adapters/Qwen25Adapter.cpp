// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/adapters/Qwen25Adapter.h"
#include "qai_forge/utils/Logger.h"
#include <sstream>
#include <regex>

// ─────────────────────────────────────────────────────────────────────────────
// Qwen25Adapter — Vision Preprocessing
//
// Qwen 2.5-VL uses the following image token format:
//   <|vision_start|><|image_pad|><|vision_end|>
// The image URL is passed separately via the VLM pipeline (not embedded in text).
// ─────────────────────────────────────────────────────────────────────────────
json Qwen25Adapter::preprocessVision(const json& messages) const {
    json processed = json::array();

    for (const auto& msg : messages) {
        if (!msg.is_object()) { processed.push_back(msg); continue; }

        std::string role = msg.value("role", "");
        const auto& content = msg["content"];

        // If content is a string, pass through unchanged
        if (content.is_string()) {
            processed.push_back(msg);
            continue;
        }

        // If content is an array of parts, process image_url parts
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
                        // Inject Qwen 2.5-VL image placeholder token
                        text_content += "<|vision_start|><|image_pad|><|vision_end|>";
                    }
                }
            }

            json new_msg = {
                {"role", role},
                {"content", text_content}
            };
            // Attach image URLs as metadata for the VLM pipeline
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
// Qwen25Adapter — Tool Instructions
//
// Qwen 2.5 uses Hermes-style tool definitions injected into the system prompt.
// ─────────────────────────────────────────────────────────────────────────────
std::string Qwen25Adapter::formatToolInstructions(const json& tools) const {
    if (tools.empty() || !tools.is_array()) return "";

    std::ostringstream oss;
    oss << "\n\n# Tools\n\n";
    oss << "You may call one or more functions to assist with the user query.\n\n";
    oss << "You are provided with function signatures within <tools></tools> XML tags:\n";
    oss << "<tools>\n";

    for (const auto& tool : tools) {
        if (!tool.is_object()) continue;
        std::string type = tool.value("type", "");
        if (type != "function") continue;

        auto func = tool.value("function", json::object());
        json tool_def = {
            {"type", "function"},
            {"function", func}
        };
        oss << tool_def.dump() << "\n";
    }

    oss << "</tools>\n\n";
    oss << "For each function call, return a json object with function name and arguments ";
    oss << "within <tool_call></tool_call> XML tags:\n";
    oss << "<tool_call>\n{\"name\": <function-name>, \"arguments\": <args-json-object>}\n</tool_call>";

    return oss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// Qwen25Adapter — Parse Tool Calls
//
// Parses <tool_call>{"name": "...", "arguments": {...}}</tool_call> from response.
// ─────────────────────────────────────────────────────────────────────────────
json Qwen25Adapter::parseToolCalls(const std::string& response_text) const {
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
            json arguments = call.value("arguments", json::object());

            // Convert arguments to string if it's already an object
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
            LOG_WARN("[Qwen25Adapter] Failed to parse tool_call: " << e.what());
        }
    }

    return tool_calls;
}

// ─────────────────────────────────────────────────────────────────────────────
// Qwen25Adapter — Format Tool Response
// ─────────────────────────────────────────────────────────────────────────────
std::string Qwen25Adapter::formatToolResponse(const json& tool_results) const {
    std::ostringstream oss;
    for (const auto& result : tool_results) {
        if (!result.is_object()) continue;
        std::string tool_call_id = result.value("tool_call_id", "");
        std::string content = result.value("content", "");
        oss << "<tool_response>\n";
        if (!tool_call_id.empty()) oss << "{\"tool_call_id\": \"" << tool_call_id << "\", ";
        oss << "\"content\": " << json(content).dump() << "}\n";
        oss << "</tool_response>\n";
    }
    return oss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// Qwen25Adapter — Build System Prompt
// ─────────────────────────────────────────────────────────────────────────────
std::string Qwen25Adapter::buildSystemPrompt(const json& chat_template,
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
