// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

using TokenBudgetJson = nlohmann::ordered_json;

namespace TokenBudgetUtils {

struct ContextBudgetResult {
    bool ok = true;
    int http_status = 200;
    std::string error_message;
    std::string error_param;
    std::string error_code;

    int context_size = 0;
    int input_tokens = 0;
    int available_completion_tokens = 0;
    int resolved_max_output_tokens = 0;
    bool tool_response_dominates = false;
};

int estimate_tokens(const std::string& text);

int estimate_multimodal_content_tokens(const TokenBudgetJson& content);

int estimate_tool_response_tokens(const TokenBudgetJson& messages);

ContextBudgetResult resolve_context_budget(
    const std::string& model,
    const TokenBudgetJson& ancestor_messages,
    const TokenBudgetJson& current_messages,
    const std::string& instructions,
    const TokenBudgetJson& tools,
    std::optional<int> requested_max_output_tokens);

int default_max_output_tokens(int context_size);

} // namespace TokenBudgetUtils
