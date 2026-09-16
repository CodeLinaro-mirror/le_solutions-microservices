// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/adapters/ToolCallJsonUtils.h"
#include "qai_forge/utils/Logger.h"
#include <algorithm>
#include <cctype>
#include <regex>

namespace ToolCallJsonUtils {

std::size_t findBalancedJsonEnd(const std::string& text, std::size_t start) {
    if (start >= text.size()) {
        return std::string::npos;
    }

    char open = text[start];
    if (open != '{' && open != '[') {
        return std::string::npos;
    }
    char close = (open == '{') ? '}' : ']';

    int depth = 0;
    bool in_string = false;
    bool escaped = false;

    for (std::size_t i = start; i < text.size(); ++i) {
        char c = text[i];

        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }

        if (c == '"') {
            in_string = true;
            continue;
        }
        if (c == open) {
            ++depth;
        } else if (c == close) {
            --depth;
            if (depth == 0) {
                return i;
            }
        }
    }

    return std::string::npos;
}

std::size_t findNextJsonStart(const std::string& text, std::size_t from) {
    std::size_t object_pos = text.find('{', from);
    std::size_t array_pos = text.find('[', from);
    if (object_pos == std::string::npos) {
        return array_pos;
    }
    if (array_pos == std::string::npos) {
        return object_pos;
    }
    return std::min(object_pos, array_pos);
}

namespace {

std::string stripCodeFences(const std::string& text) {
    std::string result = text;

    // Strip a leading ```json / ``` fence line.
    static const std::regex leading_fence(R"(^```(?:json)?\s*\n?)",
                                           std::regex::ECMAScript);
    result = std::regex_replace(result, leading_fence, "", std::regex_constants::format_first_only);

    // Strip a trailing ``` fence.
    static const std::regex trailing_fence(R"(\n?```\s*$)", std::regex::ECMAScript);
    result = std::regex_replace(result, trailing_fence, "", std::regex_constants::format_first_only);

    return result;
}

std::string collapseDoubledQuotesAroundKeys(const std::string& text) {
    // Fixes patterns like {""location"": "Paris"} -> {"location": "Paris"}
    // Uses a custom raw-string delimiter (REGEX) since the pattern itself
    // contains `)"` sequences that would otherwise prematurely terminate a
    // plain R"(...)" raw string literal.
    static const std::regex doubled_key_quotes(
        R"REGEX(([{,]\s*)""([^"]+?)""\s*:)REGEX",
        std::regex::ECMAScript);
    return std::regex_replace(text, doubled_key_quotes, "$1\"$2\":");
}

}  // namespace

json parseJsonWithRepair(const std::string& candidate) {
    // Attempt 1: parse as-is.
    try {
        return json::parse(candidate);
    } catch (...) {
        // fall through
    }

    // Attempt 2: strip markdown code fences.
    std::string repaired = stripCodeFences(candidate);
    try {
        return json::parse(repaired);
    } catch (...) {
        // fall through
    }

    // Attempt 3: collapse doubled quotes around keys.
    repaired = collapseDoubledQuotesAroundKeys(repaired);
    try {
        return json::parse(repaired);
    } catch (...) {
        // fall through
    }

    // Attempt 4: trim up to two trailing extra closing braces/brackets
    // (handles a single spurious extra closer inserted before the final
    // legitimate closer).
    std::string trimmed = repaired;
    for (int attempt = 0; attempt < 2; ++attempt) {
        // Remove trailing whitespace first.
        while (!trimmed.empty() &&
               std::isspace(static_cast<unsigned char>(trimmed.back()))) {
            trimmed.pop_back();
        }
        if (trimmed.empty()) {
            break;
        }
        char last = trimmed.back();
        if (last != '}' && last != ']') {
            break;
        }
        // Try removing one closing delimiter from just before the end and
        // re-parse — this targets a single spurious extra closer inserted
        // before the final legitimate closer(s).
        if (trimmed.size() >= 2) {
            std::string candidate_without_extra =
                trimmed.substr(0, trimmed.size() - 1);
            try {
                return json::parse(candidate_without_extra);
            } catch (...) {
                // Keep trying: drop the char and continue the loop in case
                // there are two extra closers stacked up.
                trimmed.pop_back();
                continue;
            }
        }
        break;
    }

    // All repair attempts failed — rethrow via a fresh parse to surface a
    // meaningful exception/message to the caller.
    return json::parse(candidate);
}

bool tryParseJsonWithRepair(const std::string& candidate, json& out) {
    try {
        out = parseJsonWithRepair(candidate);
        return true;
    } catch (const std::exception& e) {
        LOG_WARN("[ToolCallJsonUtils] Failed to parse JSON candidate even "
                 "after repair attempts: " << e.what());
        return false;
    }
}

std::vector<json> extractAllJsonObjects(const std::string& text) {
    std::vector<json> results;

    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t start = findNextJsonStart(text, pos);
        if (start == std::string::npos) {
            break;
        }

        std::size_t end = findBalancedJsonEnd(text, start);
        if (end == std::string::npos) {
            // Unbalanced from this start position — try repair on the
            // remainder of the string from `start` to end-of-text, since
            // the model may have emitted a truncated/malformed closer.
            std::string tail = text.substr(start);
            json parsed;
            if (tryParseJsonWithRepair(tail, parsed)) {
                results.push_back(std::move(parsed));
            }
            // Nothing more to scan reliably after an unbalanced tail.
            break;
        }

        std::string candidate = text.substr(start, end - start + 1);
        json parsed;
        if (tryParseJsonWithRepair(candidate, parsed)) {
            results.push_back(std::move(parsed));
        }

        // Resume scanning immediately after this candidate, whether or not
        // it parsed successfully, to support multiple back-to-back JSON
        // objects (e.g. multiple untagged tool calls).
        pos = end + 1;
    }

    return results;
}

}  // namespace ToolCallJsonUtils
