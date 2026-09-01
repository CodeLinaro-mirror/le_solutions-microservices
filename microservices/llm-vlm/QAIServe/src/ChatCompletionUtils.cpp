// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ChatCompletionUtils.h"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
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

        // Full content participates in the hash for every role, including
        // assistant — matches the legacy Python ConversationUtils behavior
        // (calculate_hash_for_specific_messages) exactly: role + full content
        // + tool-call info. Assistant content is NOT stripped down to a role
        // marker; the client's replayed/continued conversation must match the
        // server's own generated content byte-for-byte for a hash match.
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

    // Identify complete pairs. Per identifyCompletePairs(), only sequences
    // that culminate in an assistant message with real (non-null) content
    // are ever returned here — an in-progress or unterminated tool-calling
    // loop is excluded entirely, mirroring the legacy Python behavior.
    std::vector<MessagePair> pairs = identifyCompletePairs(non_system_messages);

    // Exclude last pair if requested (used internally; the primary
    // session-lookup path in ChatCompletionStore always calls with
    // exclude_last_pair=false and instead relies on the session's
    // rotating continuation_hash/retry_candidate_hash for matching).
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

std::string buildRequestSignature(const json& request_body, const json& messages) {
    // Direct port of Python's SessionManager.build_request_signature():
    // start from the full request body, strip fields that legitimately vary
    // between an original request and an idempotent retry of it without
    // changing the request's meaning (stream/user/store), substitute in the
    // canonical messages array, then serialize with alphabetically-sorted
    // keys so the same logical request always produces the same string
    // regardless of client-side key ordering.
    json payload = request_body.is_object() ? request_body : json::object();
    payload.erase("stream");
    payload.erase("user");
    payload.erase("store");
    payload["messages"] = messages;

    // nlohmann::ordered_json preserves insertion order rather than sorting
    // keys automatically (unlike Python's json.dumps(..., sort_keys=True)).
    // Rebuild into a plain (alphabetically-ordered) nlohmann::json object so
    // dump() emits keys in sorted order, matching Python's determinism.
    nlohmann::json sorted_payload = payload;
    return sorted_payload.dump();
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

            // Collect the ENTIRE assistant/tool sequence following this user
            // message (which may span multiple tool-calling round-trips:
            // assistant(tool_calls) -> tool -> assistant(tool_calls) -> tool -> ...)
            // up to the next user message. This mirrors the legacy Python
            // logic in ConversationUtils.calculate_conversation_hash, which
            // collects the full assistant_sequence before deciding whether
            // the pair is complete.
            std::vector<json> assistant_sequence;
            while (i < messages.size()) {
                const auto& next_msg = messages[i];
                std::string next_role = next_msg.is_object()
                    ? safeGet<std::string>(next_msg, "role", "")
                    : "";
                if (next_role == "user") {
                    break;
                }
                assistant_sequence.push_back(next_msg);
                ++i;
            }

            if (!assistant_sequence.empty()) {
                // A pair is only COMPLETE if the tool-calling loop (if any)
                // has actually concluded with a real final answer — i.e. the
                // LAST assistant message in the sequence has non-null,
                // non-empty content. A trailing assistant message that is
                // purely a tool_calls stub (content == null/empty) means the
                // loop is still in progress or ended without a final answer;
                // such a sequence must NOT be counted as complete, matching
                // Python's `final_assistant` search (search from the end for
                // the first assistant message with non-empty content).
                const json* final_assistant = nullptr;
                for (auto it = assistant_sequence.rbegin();
                     it != assistant_sequence.rend(); ++it) {
                    const json& seq_msg = *it;
                    if (seq_msg.is_object()
                        && safeGet<std::string>(seq_msg, "role", "") == "assistant"
                        && !getContentString(seq_msg).empty()) {
                        final_assistant = &seq_msg;
                        break;
                    }
                }

                if (final_assistant != nullptr) {
                    for (const auto& seq_msg : assistant_sequence) {
                        pair.messages.push_back(seq_msg);
                    }
                    pairs.push_back(pair);
                }
                // else: incomplete tool-calling sequence (no final answer
                // yet) — excluded from pairs entirely, per Python behavior.
            }
            // else: user message with no assistant response yet — this is
            // the current pending turn, not a pair. Skip (do not push).
        } else {
            ++i;
        }
    }

    return pairs;
}

} // namespace ChatCompletionUtils
