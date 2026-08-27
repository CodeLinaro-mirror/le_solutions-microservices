// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// ResponsesUtils.cpp — Shared helpers for HTTP and WebSocket Responses API
//
// Implements the utility functions declared in ResponsesUtils.h.
// Used by both ResponsesController (HTTP) and WsResponsesController (WebSocket).
// ─────────────────────────────────────────────────────────────────────────────

#include "ResponsesUtils.h"
#include <algorithm>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <random>
#include <regex>
#include <vector>

namespace ResponsesUtils {
namespace {

bool is_vlm_image_type(const std::string& type) {
    return type == "image_url" || type == "input_image";
}

std::string image_url_from_part(const json& part) {
    if (!part.is_object()) {
        return "";
    }
    if (!is_vlm_image_type(part.value("type", ""))) {
        return "";
    }

    json image_url = part.value("image_url", json(nullptr));
    if (image_url.is_string()) {
        return image_url.get<std::string>();
    }
    if (image_url.is_object()) {
        return image_url.value("url", "");
    }
    return "";
}

std::string latest_image_url_from_messages(const json& messages) {
    if (!messages.is_array()) {
        return "";
    }

    for (auto msg_it = messages.rbegin(); msg_it != messages.rend(); ++msg_it) {
        if (!msg_it->is_object() || !msg_it->contains("content")) {
            continue;
        }
        const json& content = (*msg_it)["content"];
        if (!content.is_array()) {
            continue;
        }

        for (auto part_it = content.rbegin();
             part_it != content.rend();
             ++part_it) {
            std::string url = image_url_from_part(*part_it);
            if (!url.empty()) {
                return url;
            }
        }
    }
    return "";
}

json convert_vlm_content_text_parts(const json& content) {
    if (content.is_string()) {
        return content;
    }
    if (!content.is_array()) {
        return content.is_null() ? json("") : content;
    }

    json parts = json::array();
    for (const auto& part : content) {
        if (!part.is_object()) {
            continue;
        }

        std::string type = part.value("type", "");
        if (type == "input_text" || type == "text" || type == "output_text") {
            parts.push_back({{"type", "text"},
                             {"text", part.value("text", "")}});
        } else if (!is_vlm_image_type(type)) {
            parts.push_back(part);
        }
    }
    return parts;
}

std::string text_runtime_content(const json& content) {
    if (content.is_string()) {
        return content.get<std::string>();
    }
    if (!content.is_array()) {
        return content.is_null() ? std::string() : content.dump();
    }

    std::ostringstream text;
    bool first = true;
    for (const auto& part : content) {
        std::string part_text;
        if (part.is_string()) {
            part_text = part.get<std::string>();
        } else if (part.is_object()) {
            std::string type = part.value("type", "");
            if (is_vlm_image_type(type)) {
                continue;
            }
            if (type == "input_text" || type == "text"
                || type == "output_text" || part.contains("text")) {
                part_text = part.value("text", "");
            }
        }

        if (part_text.empty()) {
            continue;
        }
        if (!first) {
            text << "\n";
        }
        text << part_text;
        first = false;
    }
    return text.str();
}

bool build_text_runtime_message(const json& message, json& runtime_message) {
    if (!message.is_object()) {
        return false;
    }

    runtime_message = message;
    runtime_message.erase("_thinking_content");
    bool has_tool_calls =
        runtime_message.contains("tool_calls")
        && runtime_message["tool_calls"].is_array()
        && !runtime_message["tool_calls"].empty();
    bool is_tool_result =
        runtime_message.value("role", "") == "tool"
        && runtime_message.contains("tool_call_id");

    if (runtime_message.contains("content")) {
        runtime_message["content"] =
            text_runtime_content(runtime_message["content"]);
    }

    std::string content = runtime_message.value("content", "");
    return !content.empty() || has_tool_calls || is_tool_result;
}

void attach_image_to_message(json& message, const std::string& image_url) {
    if (image_url.empty() || !message.is_object()) {
        return;
    }

    json image_part = {
        {"type", "image_url"},
        {"image_url", {{"url", image_url}}}
    };

    if (!message.contains("content")) {
        message["content"] = json::array({image_part});
        return;
    }

    json& content = message["content"];
    if (content.is_string()) {
        std::string text = content.get<std::string>();
        content = json::array();
        if (!text.empty()) {
            content.push_back({{"type", "text"}, {"text", text}});
        }
        content.push_back(std::move(image_part));
        return;
    }

    if (!content.is_array()) {
        content = json::array({image_part});
        return;
    }

    content.push_back(std::move(image_part));
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// current_unix_time
// ─────────────────────────────────────────────────────────────────────────────
int current_unix_time() {
    return static_cast<int>(
        std::chrono::system_clock::now().time_since_epoch().count() / 1000000000LL);
}

// ─────────────────────────────────────────────────────────────────────────────
// generate_response_id
// ─────────────────────────────────────────────────────────────────────────────
std::string generate_response_id() {
    static std::mt19937_64 rng(std::random_device{}());
    std::ostringstream oss;
    oss << "resp_" << std::hex << std::setw(16) << std::setfill('0') << rng();
    return oss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// input_to_messages
//
// Converts Responses API `input` to OpenAI-format messages array.
// Handles all input formats defined in the Responses API spec.
// ─────────────────────────────────────────────────────────────────────────────
json input_to_messages(const json& input, const std::string& system_prompt) {
    json messages = json::array();

    if (!system_prompt.empty()) {
        messages.push_back({{"role", "system"}, {"content", system_prompt}});
    }

    if (input.is_string()) {
        messages.push_back({{"role", "user"}, {"content", input.get<std::string>()}});
        return messages;
    }

    if (!input.is_array()) return messages;

    for (const auto& item : input) {
        if (item.is_string()) {
            messages.push_back({{"role", "user"}, {"content", item.get<std::string>()}});
            continue;
        }
        if (!item.is_object()) continue;

        std::string item_type = item.value("type", "");

        // function_call_output — tool result from a previous turn
        if (item_type == "function_call_output") {
            messages.push_back({
                {"role",         "tool"},
                {"tool_call_id", item.value("call_id", "")},
                {"content",      item.value("output", "")}
            });
            continue;
        }

        // function_call — assistant tool call from a previous turn
        if (item_type == "function_call") {
            json tool_call = {
                {"id",   item.value("call_id", item.value("id", ""))},
                {"type", "function"},
                {"function", {
                    {"name",      item.value("name", "")},
                    {"arguments", item.value("arguments", "{}")}
                }}
            };
            messages.push_back({
                {"role",       "assistant"},
                {"content",    nullptr},
                {"tool_calls", json::array({tool_call})}
            });
            continue;
        }

        // Standard message or content part. Preserve content arrays as-is so
        // VLM image_url parts survive the shared HTTP/WebSocket conversion.
        std::string role = item.value("role", "user");
        if (item.contains("content")) {
            messages.push_back({{"role", role}, {"content", item["content"]}});
        } else if (item.contains("text")) {
            messages.push_back({{"role", role}, {"content", item["text"]}});
        }
    }

    return messages;
}

// ─────────────────────────────────────────────────────────────────────────────
// build_text_runtime_messages
// ─────────────────────────────────────────────────────────────────────────────
json build_text_runtime_messages(const json& messages) {
    json runtime_messages = json::array();
    if (!messages.is_array()) {
        return runtime_messages;
    }

    for (const auto& message : messages) {
        json runtime_message;
        if (build_text_runtime_message(message, runtime_message)) {
            runtime_messages.push_back(std::move(runtime_message));
        }
    }

    return runtime_messages;
}

// ─────────────────────────────────────────────────────────────────────────────
// build_text_runtime_messages_with_sources
// ─────────────────────────────────────────────────────────────────────────────
json build_text_runtime_messages_with_sources(
    const json& messages,
    const std::vector<std::string>& message_source_ids,
    std::vector<std::string>& runtime_message_source_ids) {
    json runtime_messages = json::array();
    runtime_message_source_ids.clear();
    if (!messages.is_array()) {
        return runtime_messages;
    }

    std::size_t index = 0;
    for (const auto& message : messages) {
        json runtime_message;
        if (build_text_runtime_message(message, runtime_message)) {
            runtime_messages.push_back(std::move(runtime_message));
            runtime_message_source_ids.push_back(
                index < message_source_ids.size()
                    ? message_source_ids[index]
                    : std::string());
        }
        ++index;
    }

    return runtime_messages;
}

// ─────────────────────────────────────────────────────────────────────────────
// build_vlm_runtime_messages
// ─────────────────────────────────────────────────────────────────────────────
json build_vlm_runtime_messages(const json& current_messages,
                                const json& ancestor_messages) {
    json runtime_messages = json::array();
    if (!current_messages.is_array()) {
        return runtime_messages;
    }

    std::string image_url = latest_image_url_from_messages(current_messages);
    if (image_url.empty()) {
        image_url = latest_image_url_from_messages(ancestor_messages);
    }

    int last_user_index = -1;
    for (const auto& message : current_messages) {
        if (!message.is_object()) {
            continue;
        }

        json runtime_message = message;
        if (runtime_message.contains("content")) {
            runtime_message["content"] =
                convert_vlm_content_text_parts(runtime_message["content"]);
        }

        if (runtime_message.value("role", "") == "user") {
            last_user_index = static_cast<int>(runtime_messages.size());
        }
        runtime_messages.push_back(std::move(runtime_message));
    }

    if (last_user_index >= 0 && !image_url.empty()) {
        attach_image_to_message(
            runtime_messages[static_cast<std::size_t>(last_user_index)],
            image_url);
    }

    return runtime_messages;
}

// ─────────────────────────────────────────────────────────────────────────────
// extract_mcp_tool_requests
// ─────────────────────────────────────────────────────────────────────────────
std::vector<McpToolRequest> extract_mcp_tool_requests(const json& tools) {
    std::vector<McpToolRequest> requests;
    if (!tools.is_array()) return requests;

    for (const auto& tool : tools) {
        if (!tool.is_object()) continue;
        if (tool.value("type", "") != "mcp") continue;

        McpToolRequest req;
        req.server_label = tool.value("server_label", "");
        req.server_url   = tool.value("server_url", "");

        if (tool.contains("allowed_tools") && tool["allowed_tools"].is_array()) {
            for (const auto& t : tool["allowed_tools"]) {
                if (t.is_string()) req.allowed_tools.push_back(t.get<std::string>());
            }
        }
        requests.push_back(req);
    }
    return requests;
}

// ─────────────────────────────────────────────────────────────────────────────
// strip_tool_call_protocol_text
// ─────────────────────────────────────────────────────────────────────────────
std::string strip_tool_call_protocol_text(const std::string& text) {
    static const std::regex tool_call_re(
        R"(<tool_call>\s*[\s\S]*?\s*</tool_call>)",
        std::regex::ECMAScript);

    std::string cleaned = std::regex_replace(text, tool_call_re, "");
    const auto begin = std::find_if_not(
        cleaned.begin(), cleaned.end(), [](unsigned char c) {
            return std::isspace(c);
        });
    const auto end = std::find_if_not(
        cleaned.rbegin(), cleaned.rend(), [](unsigned char c) {
            return std::isspace(c);
        }).base();

    if (begin >= end) {
        return "";
    }
    return std::string(begin, end);
}

// ─────────────────────────────────────────────────────────────────────────────
// build_output_array
//
// Reasoning summaries are not exposed in output[]; reasoning effort only
// affects model-side thinking budget and usage accounting.
// ─────────────────────────────────────────────────────────────────────────────
json build_output_array(const StandardResponse& result,
                         const std::vector<McpCallRecord>& mcp_records) {
    json output = json::array();

    // MCP call records come first (they happened before the final answer)
    for (const auto& record : mcp_records) {
        output.push_back(record.to_output_item());
    }

    // ── Message output item (answer text only — no reasoning here) ────────────
    const bool has_tool_calls =
        result.tool_calls.has_value() && !result.tool_calls.value().empty();
    std::string cleaned_content = result.content.has_value()
        ? strip_tool_call_protocol_text(result.content.value())
        : "";

    json content_array = json::array();
    if (!cleaned_content.empty()) {
        content_array.push_back({
            {"type", "output_text"},
            {"text", cleaned_content}
        });
    }

    if (!content_array.empty() || !has_tool_calls) {
        output.push_back({
            {"type",    "message"},
            {"id",      "msg_" + result.id},
            {"role",    "assistant"},
            {"content", content_array},
            {"status",  "completed"}
        });
    }

    // Function call output items (non-MCP tool calls, if any)
    if (has_tool_calls) {
        for (const auto& tc : result.tool_calls.value()) {
            output.push_back({
                {"type",      "function_call"},
                {"id",        tc.value("id", "")},
                {"call_id",   tc.value("id", "")},
                {"name",      tc.value("function", json::object()).value("name", "")},
                {"arguments", tc.value("function", json::object()).value("arguments", "")},
                {"status",    "completed"}
            });
        }
    }

    return output;
}

// ─────────────────────────────────────────────────────────────────────────────
// build_response_object
// ─────────────────────────────────────────────────────────────────────────────
json build_response_object(const std::string& response_id,
                            const std::string& model,
                            const json& output,
                            const std::string& status,
                            int prompt_tokens,
                            int completion_tokens,
                            int created_at,
                            const json& error,
                            const json& incomplete_details,
                            const std::string& previous_response_id,
                            const json& metadata) {
    return {
        {"id",               response_id},
        {"object",           "response"},
        {"created_at",       created_at},
        {"model",            model},
        {"status",           status},
        {"output",           output},
        {"usage", {
            {"input_tokens",  prompt_tokens},
            {"output_tokens", completion_tokens},
            {"total_tokens",  prompt_tokens + completion_tokens}
        }},
        {"error",            error.is_null() ? json(nullptr) : error},
        {"incomplete_details", incomplete_details.is_null()
            ? json(nullptr)
            : incomplete_details},
        {"previous_response_id", previous_response_id.empty()
            ? json(nullptr)
            : json(previous_response_id)},
        {"metadata", metadata.is_null() ? json::object() : metadata}
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// synthesize_in_progress
// ─────────────────────────────────────────────────────────────────────────────
json synthesize_in_progress(const std::string& response_id,
                            const std::string& model,
                            int created_at,
                            const std::string& previous_response_id,
                            const json& metadata) {
    return {
        {"id", response_id},
        {"object", "response"},
        {"created_at", created_at},
        {"model", model},
        {"status", "in_progress"},
        {"output", json::array()},
        {"usage", nullptr},
        {"error", nullptr},
        {"incomplete_details", nullptr},
        {"previous_response_id", previous_response_id.empty()
            ? json(nullptr)
            : json(previous_response_id)},
        {"metadata", metadata.is_null() ? json::object() : metadata}
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// normalize_input_items
// ─────────────────────────────────────────────────────────────────────────────
json normalize_input_items(const std::string& response_id,
                           const json& raw_input) {
    auto make_id = [&response_id](std::size_t index) {
        std::ostringstream oss;
        oss << "item_" << response_id << "_"
            << std::setw(6) << std::setfill('0') << index;
        return oss.str();
    };

    auto user_text_item = [&make_id](std::size_t index,
                                     const std::string& text) {
        return json{
            {"id", make_id(index)},
            {"type", "message"},
            {"role", "user"},
            {"content", json::array({{
                {"type", "input_text"},
                {"text", text}
            }})}
        };
    };

    json items = json::array();
    if (raw_input.is_string()) {
        items.push_back(user_text_item(0, raw_input.get<std::string>()));
        return items;
    }
    if (!raw_input.is_array()) {
        return items;
    }

    for (std::size_t i = 0; i < raw_input.size(); ++i) {
        const auto& item = raw_input[i];
        if (item.is_string()) {
            items.push_back(user_text_item(i, item.get<std::string>()));
            continue;
        }
        if (!item.is_object()) {
            continue;
        }

        std::string type = item.value("type", "");
        bool is_message = (type == "message")
            || (type.empty() && item.contains("role"));

        if (!type.empty() && !is_message) {
            json normalized = item;
            normalized["id"] = make_id(i);
            items.push_back(std::move(normalized));
            continue;
        }

        json normalized = item;
        normalized["id"] = make_id(i);
        normalized["type"] = "message";
        std::string role = item.value("role", "user");
        normalized["role"] = role;

        json content = json::array();
        if (item.contains("content")) {
            const auto& raw_content = item["content"];
            if (raw_content.is_string()) {
                content = json::array({{
                    {"type", "input_text"},
                    {"text", raw_content.get<std::string>()}
                }});
            } else {
                content = raw_content;
            }
        } else if (item.contains("text")) {
            content = json::array({{
                {"type", "input_text"},
                {"text", item["text"]}
            }});
        }
        normalized["content"] = content;
        items.push_back(std::move(normalized));
    }

    return items;
}

// ─────────────────────────────────────────────────────────────────────────────
// paginate_input_items
// ─────────────────────────────────────────────────────────────────────────────
PaginateResult paginate_input_items(const json& items,
                                    int limit,
                                    const std::string& order,
                                    const std::string& after) {
    PaginateResult result;
    json view = items.is_array() ? items : json::array();
    if (order == "desc") {
        std::reverse(view.begin(), view.end());
    }

    std::size_t start = 0;
    if (!after.empty()) {
        bool found = false;
        for (std::size_t i = 0; i < view.size(); ++i) {
            if (view[i].is_object() && view[i].value("id", "") == after) {
                start = i + 1;
                found = true;
                break;
            }
        }
        if (!found) {
            result.error_message = "after cursor not found";
            return result;
        }
    }

    json page = json::array();
    std::size_t count = static_cast<std::size_t>(limit);
    std::size_t end = std::min(view.size(), start + count);
    for (std::size_t i = start; i < end; ++i) {
        page.push_back(view[i]);
    }

    result.ok = true;
    result.envelope = {
        {"object", "list"},
        {"data", page},
        {"first_id", page.empty() ? json(nullptr) : page.front()["id"]},
        {"last_id", page.empty() ? json(nullptr) : page.back()["id"]},
        {"has_more", end < view.size()}
    };
    return result;
}

} // namespace ResponsesUtils
