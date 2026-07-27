// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

using json = nlohmann::ordered_json;

namespace ChatCompletionUtils {

/**
 * @brief Represents a complete conversation pair (user + assistant + optional tool messages)
 */
struct MessagePair {
    std::vector<json> messages;
};

/**
 * @brief Calculate SHA-256 hash for specific messages without filtering
 *
 * Direct port of Python's calculate_hash_for_specific_messages().
 * Used for tool call hash calculation.
 *
 * @param messages Array of message objects
 * @param debug Enable debug logging
 * @return First 16 characters of SHA-256 hash
 */
std::string hashSpecificMessages(const json& messages, bool debug = false);

/**
 * @brief Calculate SHA-256 hash for conversation pairs with filtering
 *
 * Direct port of Python's calculate_conversation_hash().
 * Filters out system messages and identifies complete user-assistant pairs.
 * Used for continuation and retry candidate hashes.
 *
 * @param messages Array of message objects
 * @param exclude_last_pair If true, exclude the last incomplete pair from hash
 * @return First 16 characters of SHA-256 hash
 */
std::string hashConversationPairs(const json& messages, bool exclude_last_pair = true);

/**
 * @brief Extract content string from a message object
 *
 * Handles both string content and array content (for multimodal messages).
 *
 * @param message Message object
 * @return Content as string
 */
std::string getContentString(const json& message);

/**
 * @brief Extract tool call information from a message
 *
 * Formats tool calls as "function_name1,function_name2,..."
 *
 * @param message Message object
 * @return Comma-separated list of tool function names
 */
std::string getToolCallInfo(const json& message);

/**
 * @brief Check if a message has tool calls
 *
 * @param message Message object
 * @return True if message contains tool_calls array
 */
bool hasToolCalls(const json& message);

/**
 * @brief Safely get a value from JSON object with default
 *
 * @param obj JSON object
 * @param key Key to retrieve
 * @param default_value Default value if key doesn't exist
 * @return Value or default
 */
template<typename T>
T safeGet(const json& obj, const std::string& key, const T& default_value) {
    if (obj.is_object() && obj.contains(key)) {
        try {
            return obj[key].get<T>();
        } catch (...) {
            return default_value;
        }
    }
    return default_value;
}

/**
 * @brief Identify complete conversation pairs from messages
 *
 * A complete pair consists of:
 * - User message
 * - Assistant response (optional)
 * - Tool messages (optional, if assistant made tool calls)
 *
 * @param messages Array of message objects
 * @return Vector of MessagePair objects
 */
std::vector<MessagePair> identifyCompletePairs(const json& messages);

} // namespace ChatCompletionUtils
