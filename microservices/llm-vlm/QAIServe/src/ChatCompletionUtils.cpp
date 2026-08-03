// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ChatCompletionUtils.h"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <sstream>
#include <iomanip>
#include <drogon/drogon.h>

namespace ChatCompletionUtils {

std::string hashSpecificMessages(const json& messages, bool debug) {
    if (!messages.is_array()) {
        LOG_ERROR << "hashSpecificMessages: messages is not an array";
        return "";
    }

    std::string conversation_str;

    for (const auto& msg : messages) {
        if (!msg.is_object()) {
            continue;
        }

        std::string role = safeGet<std::string>(msg, "role", "");
        std::string content = getContentString(msg);
        std::string tool_calls_info;

        if (hasToolCalls(msg)) {
            std::string tool_info = getToolCallInfo(msg);
            if (!tool_info.empty()) {
                tool_calls_info = "|tools:" + tool_info;
            }
        }

        conversation_str += role + ":" + content + tool_calls_info + "|";
    }

    if (debug) {
        LOG_INFO << "Hash input string: " << conversation_str;
    }

    // Calculate SHA-256 hash using OpenSSL
    unsigned char hash[SHA256_DIGEST_LENGTH];
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();

    if (ctx == nullptr) {
        LOG_ERROR << "Failed to create EVP_MD_CTX";
        return "";
    }

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        LOG_ERROR << "Failed to initialize SHA-256 digest";
        EVP_MD_CTX_free(ctx);
        return "";
    }

    if (EVP_DigestUpdate(ctx, conversation_str.c_str(), conversation_str.length()) != 1) {
        LOG_ERROR << "Failed to update SHA-256 digest";
        EVP_MD_CTX_free(ctx);
        return "";
    }

    unsigned int hash_len = 0;
    if (EVP_DigestFinal_ex(ctx, hash, &hash_len) != 1) {
        LOG_ERROR << "Failed to finalize SHA-256 digest";
        EVP_MD_CTX_free(ctx);
        return "";
    }

    EVP_MD_CTX_free(ctx);

    // Convert to hex string and return first 16 characters
    std::stringstream ss;
    for (unsigned int i = 0; i < hash_len; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }

    std::string full_hash = ss.str();
    return full_hash.substr(0, 16);
}

std::string hashConversationPairs(const json& messages, bool exclude_last_pair) {
    if (!messages.is_array()) {
        LOG_ERROR << "hashConversationPairs: messages is not an array";
        return "";
    }

    // Filter out system messages
    json non_system_messages = json::array();
    for (const auto& msg : messages) {
        if (msg.is_object() && safeGet<std::string>(msg, "role", "") != "system") {
            non_system_messages.push_back(msg);
        }
    }

    // Identify complete pairs
    std::vector<MessagePair> pairs = identifyCompletePairs(non_system_messages);

    // Exclude last pair if requested
    if (exclude_last_pair && !pairs.empty()) {
        pairs.pop_back();
    }

    // No complete pairs to hash - return empty string rather than hashing
    // an empty array, which would produce the same constant hash for every
    // such call and collide across unrelated sessions.
    if (pairs.empty()) {
        return "";
    }

    // Flatten pairs into single array
    json flattened_messages = json::array();
    for (const auto& pair : pairs) {
        for (const auto& msg : pair.messages) {
            flattened_messages.push_back(msg);
        }
    }

    // Calculate hash
    return hashSpecificMessages(flattened_messages);
}

std::string getContentString(const json& message) {
    if (!message.is_object() || !message.contains("content")) {
        return "";
    }

    const auto& content = message["content"];

    // Handle string content
    if (content.is_string()) {
        return content.get<std::string>();
    }

    // Handle array content (multimodal messages)
    if (content.is_array()) {
        std::string result;
        for (const auto& item : content) {
            if (item.is_object() && item.contains("type")) {
                std::string type = item["type"].get<std::string>();
                if (type == "text" && item.contains("text")) {
                    result += item["text"].get<std::string>();
                } else if (type == "image_url" && item.contains("image_url")) {
                    // For images, just note that an image is present
                    result += "[image]";
                }
            }
        }
        return result;
    }

    return "";
}

std::string getToolCallInfo(const json& message) {
    if (!message.is_object() || !message.contains("tool_calls")) {
        return "";
    }

    const auto& tool_calls = message["tool_calls"];
    if (!tool_calls.is_array() || tool_calls.empty()) {
        return "";
    }

    std::vector<std::string> function_names;
    for (const auto& tool_call : tool_calls) {
        if (tool_call.is_object() && tool_call.contains("function")) {
            const auto& function = tool_call["function"];
            if (function.is_object() && function.contains("name")) {
                function_names.push_back(function["name"].get<std::string>());
            }
        }
    }

    // Join function names with commas
    std::string result;
    for (size_t i = 0; i < function_names.size(); ++i) {
        if (i > 0) {
            result += ",";
        }
        result += function_names[i];
    }

    return result;
}

bool hasToolCalls(const json& message) {
    if (!message.is_object() || !message.contains("tool_calls")) {
        return false;
    }

    const auto& tool_calls = message["tool_calls"];
    return tool_calls.is_array() && !tool_calls.empty();
}

std::vector<MessagePair> identifyCompletePairs(const json& messages) {
    std::vector<MessagePair> pairs;

    if (!messages.is_array()) {
        return pairs;
    }

    size_t i = 0;
    while (i < messages.size()) {
        const auto& msg = messages[i];

        if (!msg.is_object()) {
            ++i;
            continue;
        }

        std::string role = safeGet<std::string>(msg, "role", "");

        if (role == "user") {
            MessagePair pair;
            pair.messages.push_back(msg);
            ++i;

            // Look for assistant response
            if (i < messages.size()) {
                const auto& next_msg = messages[i];
                if (next_msg.is_object() && safeGet<std::string>(next_msg, "role", "") == "assistant") {
                    pair.messages.push_back(next_msg);
                    ++i;

                    // Check for tool messages following the assistant response
                    while (i < messages.size()) {
                        const auto& tool_msg = messages[i];
                        if (tool_msg.is_object() && safeGet<std::string>(tool_msg, "role", "") == "tool") {
                            pair.messages.push_back(tool_msg);
                            ++i;
                        } else {
                            break;
                        }
                    }
                }
            }

            pairs.push_back(pair);
        } else {
            ++i;
        }
    }

    return pairs;
}

} // namespace ChatCompletionUtils
