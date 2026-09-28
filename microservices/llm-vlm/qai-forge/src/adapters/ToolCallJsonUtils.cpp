// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/adapters/ToolCallJsonUtils.h"
#include "qai_forge/utils/Logger.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#include <regex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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

// ─────────────────────────────────────────────────────────────────────────────
// Cross-adapter tool-call post-processing helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

std::string toLower(const std::string& value) {
    std::string lower;
    lower.reserve(value.size());
    for (const unsigned char ch : value) {
        lower.push_back(static_cast<char>(std::tolower(ch)));
    }
    return lower;
}

// Builds a lowercased-name -> (original_name, expected_param_set) map from
// an OpenAI-format tools array. Shared by normalizeToolName() and
// extractSchemaGuidedToolCalls().
std::unordered_map<std::string, std::pair<std::string, std::set<std::string>>>
buildToolSpecs(const json& tools) {
    std::unordered_map<std::string, std::pair<std::string, std::set<std::string>>> specs;
    if (!tools.is_array()) {
        return specs;
    }
    for (const auto& tool : tools) {
        if (!tool.is_object()) {
            continue;
        }
        json function = tool.value("function", json::object());
        std::string name = function.value("name", "");
        if (name.empty()) {
            continue;
        }
        json parameters = function.value("parameters", json::object());
        std::set<std::string> properties;
        if (parameters.is_object() && parameters.contains("properties") &&
            parameters["properties"].is_object()) {
            for (const auto& item : parameters["properties"].items()) {
                properties.insert(item.key());
            }
        }
        specs[toLower(name)] = {name, std::move(properties)};
    }
    return specs;
}

std::string generateToolCallId() {
    static const char kHexDigits[] = "0123456789abcdef";
    std::string id = "call_";
    // Not cryptographically random, but sufficient for de-duplication of
    // recovered calls within a single response — matches the informal
    // "call_<hex>" convention used elsewhere in qai-forge adapters.
    std::hash<std::string> hasher;
    std::size_t seed = hasher(std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    for (int i = 0; i < 24; ++i) {
        id.push_back(kHexDigits[seed & 0xF]);
        seed >>= 4;
        if (seed == 0) {
            seed = hasher(id);
        }
    }
    return id;
}

// Recurses through `value` looking for tool-call-shaped JSON according to
// `specs`, collecting every match found (not just the first), so multiple
// back-to-back native calls in the same response are all recovered.
void findAllSchemaGuidedCalls(
    const json& value,
    const std::unordered_map<std::string, std::pair<std::string, std::set<std::string>>>& specs,
    std::vector<std::pair<std::string, json>>& results) {
    if (value.is_array()) {
        for (const auto& item : value) {
            findAllSchemaGuidedCalls(item, specs, results);
        }
        return;
    }

    if (!value.is_object()) {
        return;
    }

    for (const auto& entry : value.items()) {
        const std::string& key = entry.key();
        const json& item = entry.value();

        // Convention 2: tool name used as the dict key itself.
        auto key_it = specs.find(toLower(key));
        if (key_it != specs.end() && item.is_object()) {
            const auto& expected = key_it->second.second;
            bool matches = expected.empty();
            if (!matches) {
                for (const auto& arg_entry : item.items()) {
                    if (expected.count(arg_entry.key())) {
                        matches = true;
                        break;
                    }
                }
            }
            if (matches || item.empty()) {
                results.emplace_back(key_it->second.first, item);
            }
        }

        // Convention 1: tool name appears as a string value.
        if (item.is_string()) {
            auto value_it = specs.find(toLower(item.get<std::string>()));
            if (value_it != specs.end()) {
                const auto& expected = value_it->second.second;
                bool found_sibling = false;
                for (const auto& sibling_entry : value.items()) {
                    if (sibling_entry.key() == key ||
                        !sibling_entry.value().is_object()) {
                        continue;
                    }
                    const json& sibling = sibling_entry.value();
                    bool matches = expected.empty();
                    if (!matches) {
                        for (const auto& arg_entry : sibling.items()) {
                            if (expected.count(arg_entry.key())) {
                                matches = true;
                                break;
                            }
                        }
                    }
                    if (matches) {
                        results.emplace_back(value_it->second.first, sibling);
                        found_sibling = true;
                    }
                }
                if (!found_sibling) {
                    json flattened = json::object();
                    for (const auto& sibling_entry : value.items()) {
                        if (sibling_entry.key() != key &&
                            expected.count(sibling_entry.key())) {
                            flattened[sibling_entry.key()] = sibling_entry.value();
                        }
                    }
                    if (!flattened.empty() || expected.empty()) {
                        results.emplace_back(value_it->second.first, flattened);
                    }
                }
            }
        }
    }

    // Recurse into nested structures regardless of whether this object
    // itself matched, so calls nested inside e.g. a wrapper object are
    // still discovered alongside sibling matches at this level.
    for (const auto& entry : value.items()) {
        if (entry.value().is_object() || entry.value().is_array()) {
            findAllSchemaGuidedCalls(entry.value(), specs, results);
        }
    }
}

}  // namespace

bool isEmptyToolCallResponse(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    json parsed;
    try {
        parsed = json::parse(text);
    } catch (...) {
        return false;
    }
    return parsed.is_object() && parsed.contains("tool_calls") &&
           parsed["tool_calls"].is_array() && parsed["tool_calls"].empty();
}

std::optional<std::string> normalizeToolName(const std::string& called_name,
                                             const json& tools) {
    if (called_name.empty()) {
        return std::nullopt;
    }
    auto specs = buildToolSpecs(tools);
    auto it = specs.find(toLower(called_name));
    if (it == specs.end()) {
        return std::nullopt;
    }
    return it->second.first;
}

json extractSchemaGuidedToolCalls(const std::string& text, const json& tools) {
    json result = json::array();
    auto specs = buildToolSpecs(tools);
    if (specs.empty() || text.empty()) {
        return result;
    }

    std::vector<json> candidates = extractAllJsonObjects(text);
    std::unordered_set<std::string> seen;

    for (const auto& candidate : candidates) {
        std::vector<std::pair<std::string, json>> matches;
        findAllSchemaGuidedCalls(candidate, specs, matches);
        for (auto& match : matches) {
            std::string dedup_key;
            try {
                dedup_key = match.first + "|" + match.second.dump();
            } catch (...) {
                dedup_key = match.first;
            }
            if (!seen.insert(dedup_key).second) {
                continue;
            }
            result.push_back({
                {"id", generateToolCallId()},
                {"type", "function"},
                {"function", {
                    {"name", match.first},
                    {"arguments", match.second.dump()}
                }}
            });
        }
    }

    return result;
}

}  // namespace ToolCallJsonUtils
