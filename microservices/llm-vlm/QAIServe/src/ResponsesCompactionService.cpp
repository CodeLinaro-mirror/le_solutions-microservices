// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ResponsesCompactionService.h"

#include "ResponsesConstants.h"
#include "ResponsesUtils.h"
#include "TokenBudgetUtils.h"
#include "qai_forge/managers/ModelConfigManager.h"

#include <algorithm>
#include <sstream>
#include <vector>

namespace {

using CompactResult = ResponsesCompactionService::CompactBranchResult;

CompactResult makeCompactError(int http_status,
                               const std::string& error_message) {
    CompactResult result;
    result.ok = false;
    result.compacted = false;
    result.http_status = http_status;
    result.error_message = error_message;
    return result;
}

std::string contentText(const ResponseStoreJson& content) {
    if (content.is_string()) {
        return content.get<std::string>();
    }
    if (!content.is_array()) {
        return content.is_null() ? std::string() : content.dump();
    }

    std::ostringstream oss;
    for (const auto& part : content) {
        if (!part.is_object()) {
            continue;
        }
        std::string type = part.value("type", "");
        if (type == "text" || type == "input_text" || type == "output_text") {
            oss << part.value("text", "");
        } else if (type == "image_url" || type == "input_image") {
            oss << "[Image]";
        }
    }
    return oss.str();
}

int estimateMessages(const ResponseStoreJson& messages) {
    if (!messages.is_array()) {
        return 0;
    }

    int total = 0;
    for (const auto& message : messages) {
        if (!message.is_object()) {
            continue;
        }
        total += ResponsesConstants::MESSAGE_OVERHEAD_TOKENS;
        if (message.contains("content")) {
            total += TokenBudgetUtils::estimate_multimodal_content_tokens(
                message["content"]);
        }
        if (message.contains("tool_calls")) {
            total += TokenBudgetUtils::estimate_tokens(
                message["tool_calls"].dump());
        }
    }
    return total;
}

int estimateResponse(const StoredResponse& response) {
    return estimateMessages(response.request_messages)
        + estimateMessages(response.assistant_messages);
}

void appendMessages(ResponseStoreJson& destination,
                    const ResponseStoreJson& messages) {
    if (!messages.is_array()) {
        return;
    }
    for (const auto& message : messages) {
        destination.push_back(message);
    }
}

std::string roleLabel(const std::string& role) {
    if (role == "assistant") {
        return "Assistant";
    }
    if (role == "system") {
        return "System";
    }
    return "User";
}

std::string buildSummaryPrompt(const ResponseStoreJson& messages) {
    std::ostringstream prompt;
    prompt << "Summarize this conversation concisely:\n\n";

    if (messages.is_array()) {
        for (const auto& message : messages) {
            if (!message.is_object()) {
                continue;
            }

            std::string role = message.value("role", "user");
            if (role == "tool"
                || message.value("type", "") == "function_call_output") {
                continue;
            }

            std::string text = message.contains("content")
                ? contentText(message["content"])
                : std::string();
            if (message.contains("tool_calls")) {
                text += " [Tool Calls]";
            }
            if (text.empty()) {
                continue;
            }

            prompt << roleLabel(role) << ": " << text << "\n";
        }
    }

    prompt << "\nSummary:";
    return prompt.str();
}

std::vector<StoredResponse> collectBranch(
    const std::string& branch_head_response_id,
    CompactResult& result) {
    std::vector<StoredResponse> head_to_root;
    std::string cursor = branch_head_response_id;

    while (!cursor.empty()) {
        auto response = ResponseStore::getInstance().getResponse(cursor);
        if (!response.has_value()) {
            result = makeCompactError(
                404,
                "Response " + cursor + " not found");
            return {};
        }
        if (response->status == StoredResponseStatus::InProgress) {
            result = makeCompactError(
                409,
                "Response " + cursor + " is still in progress");
            return {};
        }
        if (response->status != StoredResponseStatus::Completed) {
            result = makeCompactError(
                404,
                "Response " + cursor + " is not continuable");
            return {};
        }

        head_to_root.push_back(*response);
        cursor = response->previous_response_id;
    }

    std::reverse(head_to_root.begin(), head_to_root.end());
    return head_to_root;
}

} // namespace

ResponsesCompactionService& ResponsesCompactionService::getInstance() {
    static ResponsesCompactionService service;
    return service;
}

ResponsesCompactionService::CompactBranchResult
ResponsesCompactionService::compactBranch(
    const std::string& branch_head_response_id,
    const std::string& model,
    const SummaryGenerator& generate_summary) {

    if (!generate_summary) {
        return makeCompactError(500, "summary generator is not configured");
    }

    CompactResult collect_result;
    std::vector<StoredResponse> branch =
        collectBranch(branch_head_response_id, collect_result);
    if (!collect_result.error_message.empty()) {
        return collect_result;
    }
    if (branch.size() < 2) {
        CompactResult result;
        result.ok = true;
        result.compacted = false;
        return result;
    }

    int context_size = ModelConfigManager::getInstance().getContextSize(model);
    int keep_budget = static_cast<int>(context_size * 0.5);
    int kept_tokens = 0;
    std::size_t first_raw_index = branch.size();

    for (std::size_t i = branch.size(); i > 0; --i) {
        int turn_tokens = estimateResponse(branch[i - 1]);
        if (first_raw_index == branch.size()
            || kept_tokens + turn_tokens <= keep_budget) {
            kept_tokens += turn_tokens;
            first_raw_index = i - 1;
            continue;
        }
        break;
    }

    if (first_raw_index == 0) {
        CompactResult result;
        result.ok = true;
        result.compacted = false;
        return result;
    }

    ResponseStoreJson summary_messages = ResponseStoreJson::array();
    ResponseStoreJson retained_messages = ResponseStoreJson::array();
    for (std::size_t i = 0; i < branch.size(); ++i) {
        ResponseStoreJson& target =
            i < first_raw_index ? summary_messages : retained_messages;
        appendMessages(target, branch[i].request_messages);
        appendMessages(target, branch[i].assistant_messages);
    }

    std::string summary_prompt = buildSummaryPrompt(summary_messages);
    std::string summary_text = generate_summary(
        summary_prompt,
        TokenBudgetUtils::summary_max_output_tokens(context_size));
    if (summary_text.empty()) {
        return makeCompactError(500, "summary generation returned no text");
    }

    CompactionSummary summary;
    summary.compaction_id = ResponsesUtils::generate_compaction_id();
    summary.summarized_until_response_id =
        branch[first_raw_index - 1].response_id;
    summary.branch_head_response_id = branch.back().response_id;
    summary.summary_text = summary_text;
    summary.summary_tokens = TokenBudgetUtils::estimate_tokens(summary_text);
    summary.input_tokens_before = estimateMessages(summary_messages)
        + estimateMessages(retained_messages);
    summary.input_tokens_after = summary.summary_tokens
        + estimateMessages(retained_messages);
    summary.model = model;
    summary.created_at = ResponsesUtils::current_unix_time();

    ApplyCompactionResult applied =
        ResponseStore::getInstance().applyCompaction(
            branch.back().session_id,
            summary);
    if (!applied.ok) {
        return makeCompactError(applied.http_status, applied.error_message);
    }

    CompactResult result;
    result.ok = true;
    result.compacted = applied.applied;
    result.summary = summary;
    return result;
}
