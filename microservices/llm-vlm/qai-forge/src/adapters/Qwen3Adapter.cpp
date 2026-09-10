// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/adapters/Qwen3Adapter.h"
#include "qai_forge/adapters/ToolCallJsonUtils.h"
#include "qai_forge/utils/Logger.h"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <regex>
#include <random>

namespace {

// nlohmann::json's .value(key, default) only falls back to `default` when
// the key is absent — if the key is present but explicitly null,
// .value<std::string>() throws json::type_error.302. This helper treats an
// explicit null the same as an absent key.
std::string getStringOrDefault(const json& obj,
                                const std::string& key,
                                const std::string& def = "") {
    if (!obj.is_object() || !obj.contains(key) || obj[key].is_null()) {
        return def;
    }
    const json& val = obj[key];
    return val.is_string() ? val.get<std::string>() : def;
}

// Sequential "call_" + idx IDs collide across turns/sessions (every
// response's first tool call is always "call_0"). Generates a random
// 64-bit hex ID instead, matching the generateEventId() pattern used
// elsewhere in qai-forge (e.g. LiteRTLMOrchestrator).
std::string generateToolCallId() {
    static std::mt19937_64 rng(std::random_device{}());
    std::ostringstream oss;
    oss << "call_" << std::hex << rng();
    return oss.str();
}

bool pushToolCall(const json& call, json& tool_calls, int& idx) {
    if (!call.is_object()) {
        return false;
    }

    std::string name = call.value("name", "");
    if (name.empty() || !call.contains("arguments")) {
        return false;
    }

    const json& arguments = call["arguments"];
    std::string args_str = arguments.is_string()
        ? arguments.get<std::string>()
        : arguments.dump();

    tool_calls.push_back({
        {"id", generateToolCallId()},
        {"type", "function"},
        {"function", {
            {"name", name},
            {"arguments", args_str}
        }}
    });
    ++idx;
    return true;
}

bool pushBareToolCalls(const json& parsed, json& tool_calls, int& idx) {
    if (parsed.is_object()) {
        return pushToolCall(parsed, tool_calls, idx);
    }
    if (!parsed.is_array()) {
        return false;
    }

    bool found = false;
    for (const auto& item : parsed) {
        found = pushToolCall(item, tool_calls, idx) || found;
    }
    return found;
}

// Extracts bare (untagged) tool-call JSON from `text` using the shared
// balanced-brace/repair scanner in ToolCallJsonUtils.
bool tryParseBareToolCallJson(const std::string& text,
                              json& tool_calls,
                              int& idx) {
    std::vector<json> candidates = ToolCallJsonUtils::extractAllJsonObjects(text);
    bool found = false;
    for (const auto& parsed : candidates) {
        found = pushBareToolCalls(parsed, tool_calls, idx) || found;
    }
    return found;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Qwen3Adapter — Vision Preprocessing
//
// Emits vision_start/vision_end markers adjacently in the message text when
// the model's chat_template defines them, matching the format expected by
// the VLM pipeline's prompt parsing. Falls back to a bare "<|image|>" token
// for Qwen3-VL variants whose chat_template omits these markers.
// ─────────────────────────────────────────────────────────────────────────────
json Qwen3Adapter::preprocessVision(const json& messages,
                                     const json& chat_template) const {
    json processed = json::array();

    const bool has_vision_markers = chat_template.is_object()
        && chat_template.contains("vision_start")
        && chat_template.contains("vision_end");
    std::string vision_start = has_vision_markers
        ? chat_template.value("vision_start", "") : "";
    std::string vision_end = has_vision_markers
        ? chat_template.value("vision_end", "") : "";

    for (const auto& msg : messages) {
        if (!msg.is_object()) { processed.push_back(msg); continue; }

        std::string role = getStringOrDefault(msg, "role", "");
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
                if (type == "text" || type == "input_text") {
                    text_content += part.value("text", "");
                } else if (type == "image_url" || type == "input_image") {
                    auto image_url = part.value("image_url", json::object());
                    std::string url;
                    if (image_url.is_string()) {
                        url = image_url.get<std::string>();
                    } else if (image_url.is_object()) {
                        url = image_url.value("url", "");
                    }
                    if (!url.empty()) {
                        image_urls.push_back(url);
                        if (has_vision_markers) {
                            text_content += vision_start + vision_end;
                        } else {
                            text_content += "<|image|>";
                        }
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
    oss << "You have access to the following tools.\n\n";

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

    // Solicit bare JSON (no <tool_call> XML tag wrapper) — more reliably
    // produced by this model than the tagged format. parseToolCalls() below
    // still checks for <tool_call> tags first for backward compatibility.
    //
    // The arguments placeholder is intentionally "{...}" rather than a
    // concrete example like {"param":"value"} — a concrete key name causes
    // the model to copy it literally (e.g. emitting {"param":"Boston"}
    // instead of {"location":"Boston"}).
    oss << "When calling a tool, respond with ONLY the JSON object below and "
           "nothing else — no explanations, no markdown, no text before or "
           "after it:\n";
    oss << "{\"name\":\"<tool_name>\",\"arguments\":{...}}\n";
    oss << "Replace <tool_name> with the tool name and {...} with the actual "
           "arguments as JSON, using the exact parameter names shown in the "
           "tool signature above.\n";
    oss << "If no tool is needed to answer the question, respond with plain "
           "text only. Do NOT output JSON in that case.\n";
    oss << "If multiple tool calls are needed, output multiple complete JSON "
           "objects one after another, each on its own line, and nothing else.";

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
            int call_idx = idx;
            if (!pushToolCall(call, tool_calls, call_idx)) {
                LOG_WARN("[Qwen3Adapter] Ignoring malformed tool_call block");
            }
        } catch (const std::exception& e) {
            LOG_WARN("[Qwen3Adapter] Failed to parse tool_call: " << e.what());
        }
    }

    if (tool_calls.empty()
        && tryParseBareToolCallJson(response_text, tool_calls, idx)) {
        LOG_WARN("[Qwen3Adapter] Parsed bare JSON tool_call without "
                 "<tool_call> tags");
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
        std::string tool_call_id = getStringOrDefault(result, "tool_call_id", "");
        std::string content = getStringOrDefault(result, "content", "");
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
