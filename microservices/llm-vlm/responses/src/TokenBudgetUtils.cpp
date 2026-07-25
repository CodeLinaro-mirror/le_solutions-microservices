// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "TokenBudgetUtils.h"

#include "ResponsesConstants.h"
#include "qai_forge/adapters/ModelAdapterFactory.h"
#include "qai_forge/managers/ModelConfigManager.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace TokenBudgetUtils {
namespace {

using json = TokenBudgetJson;

std::string replaceAll(std::string text,
                       const std::string& needle,
                       const std::string& value) {
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        text.replace(pos, needle.size(), value);
        pos += value.size();
    }
    return text;
}

std::string formatTemplate(const std::string& templ,
                           int context_size,
                           int cap,
                           int requested = 0) {
    std::string result = replaceAll(
        templ, "{context_size}", std::to_string(context_size));
    result = replaceAll(result, "{cap}", std::to_string(cap));
    result = replaceAll(result, "{requested}", std::to_string(requested));
    return result;
}

json normalizeContentParts(const json& messages) {
    json normalized = json::array();
    if (!messages.is_array()) {
        return normalized;
    }

    for (const auto& message : messages) {
        if (!message.is_object()) {
            continue;
        }

        json copy = message;
        if (!copy.contains("content") || !copy["content"].is_array()) {
            normalized.push_back(std::move(copy));
            continue;
        }

        json parts = json::array();
        for (const auto& part : copy["content"]) {
            if (!part.is_object()) {
                continue;
            }

            std::string type = part.value("type", "");
            if (type == "input_text" || type == "output_text") {
                parts.push_back({{"type", "text"},
                                 {"text", part.value("text", "")}});
            } else if (type == "input_image") {
                json image_url = part.value("image_url", json(nullptr));
                if (image_url.is_string()) {
                    image_url = {{"url", image_url.get<std::string>()}};
                }
                parts.push_back({{"type", "image_url"},
                                 {"image_url", image_url}});
            } else {
                parts.push_back(part);
            }
        }
        copy["content"] = std::move(parts);
        normalized.push_back(std::move(copy));
    }

    return normalized;
}

int combined_tool_response_tokens(const json& ancestor_messages,
                                  const json& current_messages) {
    return estimate_tool_response_tokens(ancestor_messages)
        + estimate_tool_response_tokens(current_messages);
}

int estimate_message_content_tokens(const json& message) {
    if (!message.is_object() || !message.contains("content")) {
        return 0;
    }
    return estimate_multimodal_content_tokens(message["content"]);
}

} // namespace

int estimate_tokens(const std::string& text) {
    if (text.empty()) {
        return 0;
    }

    int words = 0;
    bool in_word = false;
    for (unsigned char c : text) {
        if (std::isspace(c)) {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            ++words;
        }
    }
    return static_cast<int>(words * ResponsesConstants::TOKENS_PER_WORD);
}

int estimate_multimodal_content_tokens(const json& content) {
    if (content.is_string()) {
        return estimate_tokens(content.get<std::string>());
    }
    if (!content.is_array()) {
        return content.is_null() ? 0 : estimate_tokens(content.dump());
    }

    int total = 0;
    for (const auto& part : content) {
        if (!part.is_object()) {
            continue;
        }
        std::string type = part.value("type", "");
        if (type == "text" || type == "input_text" || type == "output_text") {
            total += estimate_tokens(part.value("text", ""));
        } else if (type == "image_url" || type == "input_image") {
            total += ResponsesConstants::IMAGE_TOKEN_COST;
        }
    }
    return total;
}

int estimate_tool_response_tokens(const json& messages) {
    if (!messages.is_array()) {
        return 0;
    }

    int total = 0;
    for (const auto& message : messages) {
        if (!message.is_object()) {
            continue;
        }

        bool is_tool = message.value("role", "") == "tool"
            || message.value("type", "") == "function_call_output";
        if (!is_tool) {
            continue;
        }

        if (message.contains("content")) {
            total += estimate_multimodal_content_tokens(message["content"]);
        } else if (message.contains("output")) {
            total += estimate_tokens(message["output"].is_string()
                ? message["output"].get<std::string>()
                : message["output"].dump());
        }
    }
    return total;
}

static int estimate_prompt_tokens(
    const std::string& model,
    const json& ancestor_messages,
    const json& current_messages,
    const std::string& instructions,
    const json& tools) {

    auto& config_mgr = ModelConfigManager::getInstance();
    json chat_template = config_mgr.getChatTemplate(model);
    if (!chat_template.is_object()) {
        chat_template = json::object();  // Guard: LiteRT-LM models have no metadata.json
    }
    const auto& adapter = ModelAdapterFactory::getAdapter(model);

    int total = ResponsesConstants::MESSAGE_OVERHEAD_TOKENS;
    std::string system_content =
        adapter.buildSystemPrompt(chat_template, instructions, tools);
    if (!system_content.empty()) {
        total += ResponsesConstants::MESSAGE_OVERHEAD_TOKENS
            + estimate_tokens(system_content);
    }

    json normalized_ancestor_messages =
        normalizeContentParts(ancestor_messages);
    if (normalized_ancestor_messages.is_array()) {
        for (const auto& message : normalized_ancestor_messages) {
            if (!message.is_object()) {
                continue;
            }
            std::string role = message.value("role", "");
            if (role != "user" && role != "assistant") {
                continue;
            }
            total += ResponsesConstants::MESSAGE_OVERHEAD_TOKENS
                + estimate_message_content_tokens(message);
        }
    }

    json processed_current =
        adapter.preprocessVision(normalizeContentParts(current_messages));
    if (processed_current.is_array()) {
        for (const auto& message : processed_current) {
            if (!message.is_object()) {
                continue;
            }

            std::string role = message.value("role", "");
            if (role == "user") {
                total += ResponsesConstants::MESSAGE_OVERHEAD_TOKENS
                    + estimate_message_content_tokens(message);
            } else if (role == "tool") {
                total += ResponsesConstants::MESSAGE_OVERHEAD_TOKENS
                    + estimate_tokens(
                        adapter.formatToolResponse(json::array({message})));
            }
        }
    }

    return total;
}

static int count_input_tokens(
    const std::string& model,
    const json& ancestor_messages,
    const json& current_messages,
    const std::string& instructions,
    const json& tools) {
    return estimate_prompt_tokens(
        model, ancestor_messages, current_messages, instructions, tools);
}

ContextBudgetResult resolve_context_budget(
    const std::string& model,
    const json& ancestor_messages,
    const json& current_messages,
    const std::string& instructions,
    const json& tools,
    std::optional<int> requested_max_output_tokens) {

    ContextBudgetResult result;
    result.context_size = ModelConfigManager::getInstance().getContextSize(model);
    result.input_tokens = count_input_tokens(
        model, ancestor_messages, current_messages, instructions, tools);
    result.available_completion_tokens = std::max(
        result.context_size - result.input_tokens
            - ResponsesConstants::MAX_COMPLETION_SAFETY_MARGIN,
        0);

    int tool_tokens = combined_tool_response_tokens(
        ancestor_messages, current_messages);
    result.tool_response_dominates = tool_tokens > 0
        && tool_tokens >= std::max(result.input_tokens - tool_tokens, 0);

    if (result.available_completion_tokens
        < ResponsesConstants::MIN_USEFUL_COMPLETION_TOKENS) {
        result.ok = false;
        result.http_status = 400;
        result.error_param = "input";
        result.error_code =
            ResponsesConstants::ERROR_CODE_CONTEXT_LENGTH_EXCEEDED;
        result.error_message = formatTemplate(
            result.tool_response_dominates
                ? ResponsesConstants::PROMPT_TOO_LONG_TOOL_RESPONSE
                : ResponsesConstants::PROMPT_TOO_LONG,
            result.context_size,
            result.available_completion_tokens);
        return result;
    }

    if (requested_max_output_tokens.has_value()
        && requested_max_output_tokens.value()
            > result.available_completion_tokens) {
        result.ok = false;
        result.http_status = 400;
        result.error_param = "max_output_tokens";
        result.error_code =
            ResponsesConstants::ERROR_CODE_CONTEXT_LENGTH_EXCEEDED;
        result.error_message = formatTemplate(
            result.tool_response_dominates
                ? ResponsesConstants::CONTEXT_LENGTH_EXCEEDED_TOOL_RESPONSE
                : ResponsesConstants::CONTEXT_LENGTH_EXCEEDED,
            result.context_size,
            result.available_completion_tokens,
            requested_max_output_tokens.value());
        return result;
    }

    result.resolved_max_output_tokens = requested_max_output_tokens.value_or(
        std::min(default_max_output_tokens(result.context_size),
                 result.available_completion_tokens));
    return result;
}

int default_max_output_tokens(int context_size) {
    return static_cast<int>(context_size * 0.5);
}

} // namespace TokenBudgetUtils
