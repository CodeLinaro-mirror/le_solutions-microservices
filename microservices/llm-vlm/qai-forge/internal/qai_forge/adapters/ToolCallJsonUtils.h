// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <nlohmann/json.hpp>
#include <optional>
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

// ─────────────────────────────────────────────────────────────────────────────
// Cross-adapter tool-call post-processing helpers.
//
// These helpers operate on already-normalized OpenAI-format tool_calls
// entries ({"id","type":"function","function":{"name","arguments"}}) and
// on the caller-supplied `tools` schema array from the request, to provide
// behavior shared across all ModelAdapter::parseToolCalls() implementations:
//   - tolerating case differences in the model-emitted tool name
//   - detecting an explicit "no tool needed" signal
//   - recovering a call from schema-shaped JSON when the model dropped its
//     native tag/wrapper entirely
// ─────────────────────────────────────────────────────────────────────────────

// Returns true if `text`, parsed as JSON, is exactly {"tool_calls": []} —
// i.e. the model explicitly signalled "no tool needed" using the generic
// OpenAI-style empty-array convention rather than plain text. Callers use
// this to suppress the literal control JSON from leaking into the visible
// response content.
bool isEmptyToolCallResponse(const std::string& text);

// Case-insensitive lookup of `called_name` against the tool names declared
// in `tools` (an OpenAI-format tools array). Returns the tool's original
// (declared) casing if a case-insensitive match is found, or std::nullopt
// if `called_name` does not match any declared tool. Used to tolerate
// models that alter tool-name casing (e.g. "Get_Weather" instead of
// "get_weather") while still ensuring callers only ever see the exact name
// they declared.
std::optional<std::string> normalizeToolName(const std::string& called_name,
                                             const json& tools);

// Scans `text` for JSON values shaped like a tool call according to the
// parameter names declared in `tools`, recovering (name, arguments) pairs
// even when the model dropped its native tag/wrapper syntax entirely.
// Supports two structurally distinct conventions:
//   1. Value-based: {"name": "tool_name", "arguments": {...}}
//   2. Key-based:   {"tool_name": {...}}
// Matching on tool name is case-insensitive; the returned name uses the
// tool's original declared casing. Deduplicated by (name, arguments).
// Returns an OpenAI-format tool_calls JSON array (each entry already
// containing a generated "id"), or an empty array if nothing recognizable
// was found.
json extractSchemaGuidedToolCalls(const std::string& text, const json& tools);

}  // namespace ToolCallJsonUtils
