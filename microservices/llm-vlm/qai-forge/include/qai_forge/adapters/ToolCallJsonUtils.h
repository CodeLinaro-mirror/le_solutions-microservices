// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using json = nlohmann::ordered_json;

// ─────────────────────────────────────────────────────────────────────────────
// ToolCallJsonUtils — model-agnostic JSON extraction/repair helpers used by
// ModelAdapter::parseToolCalls() implementations to recover a valid tool-call
// JSON payload from noisy surrounding text (stray tokens, minor formatting
// slips, multiple back-to-back payloads).
// ─────────────────────────────────────────────────────────────────────────────
namespace ToolCallJsonUtils {

// Finds the index of the character that closes the balanced JSON object or
// array starting at `text[start]` ('{' or '['), skipping braces/brackets
// inside quoted string literals. Returns std::string::npos if unbalanced.
std::size_t findBalancedJsonEnd(const std::string& text, std::size_t start);

// Finds the next '{' or '[' at or after `from`. Returns std::string::npos
// if none is found.
std::size_t findNextJsonStart(const std::string& text, std::size_t from = 0);

// Parses `candidate` as JSON, applying best-effort repairs on failure
// (markdown fence stripping, doubled-quote collapsing, trimming up to two
// trailing extra closing delimiters). Throws if all attempts fail.
json parseJsonWithRepair(const std::string& candidate);

// Non-throwing wrapper around parseJsonWithRepair().
bool tryParseJsonWithRepair(const std::string& candidate, json& out);

// Scans `text` for one or more balanced JSON objects/arrays, extracting and
// parsing each (with repair) via findBalancedJsonEnd()/tryParseJsonWithRepair().
// Returns successfully parsed values in order of appearance; unparseable
// candidates are skipped. Schema-agnostic — interpretation is left to the caller.
std::vector<json> extractAllJsonObjects(const std::string& text);

}  // namespace ToolCallJsonUtils
