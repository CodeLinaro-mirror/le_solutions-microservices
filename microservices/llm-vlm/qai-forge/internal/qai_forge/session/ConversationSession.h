// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <chrono>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <nlohmann/json.hpp>

using json = nlohmann::ordered_json;

// nlohmann::json's .value(key, default) only falls back to `default` when the
// key is absent — if the key is present but explicitly null (a valid shape
// for e.g. an assistant message's "content" field when "tool_calls" is
// present), .value<std::string>() throws json::type_error.302. This inline
// helper treats an explicit null the same as an absent key. Defined here
// (not in an anonymous namespace, since this is a header) with an "session"
// prefix to avoid ODR / name-collision issues with other TUs that include
// this header alongside their own local getStringOrDefault() helpers.
inline std::string sessionGetStringOrDefault(const json& obj,
                                              const std::string& key,
                                              const std::string& def = "") {
    if (!obj.is_object() || !obj.contains(key) || obj[key].is_null()) {
        return def;
    }
    const json& val = obj[key];
    return val.is_string() ? val.get<std::string>() : def;
}

// ─────────────────────────────────────────────────────────────────────────────
// ConversationSession — Pure Data Record
//
// This is a transient data object, reconstructed fresh on every request from
// the caller-supplied transcript plus the private GenieMemoryState rehydrated
// out of ConversationMemoryCoordinator, used while preparing a Genie request
// and running post-turn processing. Persistent runtime memory lives in the
// private ConversationMemoryCoordinator tree — this object is never itself
// persisted between requests; GenieOrchestrator::createJob() and
// ::executePostTurn() each construct their own instance per call.
//
// Key design decisions:
//   - session_id identifies the active backend execution.
//   - messages contains the complete caller-supplied transcript for this turn.
//   - Thinking content is stored with a private "_thinking_content" key and
//     stripped before being sent to the model.
//   - Post-turn memory management:
//       evicted_message_count — number of oldest messages already summarized.
//       summary_content       — rolling summary of evicted messages (Slot 3).
//       facts                 — persistent key-value facts extracted from
//                               evicted messages (Slot 2).
// ─────────────────────────────────────────────────────────────────────────────

struct ConversationSession {
    std::string session_id;

    // Shared OpenAI-format message history
    std::vector<json> messages;

    // ── Post-turn memory management ───────────────────────────────────────────

    // Number of oldest messages that have been evicted (summarized).
    // Messages at indices [0, evicted_message_count) are excluded from the
    // history queue (Slot 4) — they are represented by summary_content.
    size_t evicted_message_count = 0;

    // Rolling summary of evicted messages.
    // Injected into Slot 3 of the prompt by buildContextPrompt().
    // Updated by GenieOrchestrator::postTurnProcessing() after each eviction.
    std::string summary_content;
    int summary_token_count = 0;

    // Persistent key-value facts extracted from evicted messages.
    // Injected into Slot 2 of the prompt by buildContextPrompt().
    // Updated by GenieOrchestrator::postTurnProcessing() after each eviction.
    std::unordered_map<std::string, std::string> facts;

    // Timestamp of the most recent message append. Used only to compute
    // wall-clock recency within this transient object's own lifetime; no
    // code currently reads this field, but it's retained since addMessage()
    // updates it as a natural side effect of appending.
    std::chrono::system_clock::time_point last_activity;

    explicit ConversationSession(const std::string& id)
        : session_id(id),
          last_activity(std::chrono::system_clock::now()) {}

    // Add a message to history. Returns the index of the added message.
    size_t addMessage(const json& message) {
        messages.push_back(message);
        last_activity = std::chrono::system_clock::now();
        return messages.size() - 1;
    }

    size_t addMessage(json&& message) {
        messages.push_back(std::move(message));
        last_activity = std::chrono::system_clock::now();
        return messages.size() - 1;
    }

    /**
     * @brief Get messages for the history queue slot (Slot 4).
     *
     * Returns only messages AFTER evicted_message_count — i.e., messages that
     * have NOT been evicted and summarized. Strips private "_*" keys.
     * System messages are excluded (they belong in Slot 1).
     */
    std::vector<json> getHistoryMessages() const {
        std::vector<json> result;
        for (size_t i = evicted_message_count; i < messages.size(); ++i) {
            const auto& msg = messages[i];
            std::string role = sessionGetStringOrDefault(msg, "role", "");
            if (role == "system") continue;  // System messages go in Slot 1

            json clean_msg = json::object();
            for (const auto& [k, v] : msg.items()) {
                if (k.empty() || k[0] != '_') {
                    clean_msg[k] = v;
                }
            }
            result.push_back(std::move(clean_msg));
        }
        return result;
    }

    /**
     * @brief Format persistent facts as a compact string for Slot 2.
     *
     * Returns empty string if no facts are stored.
     */
    std::string formatFacts() const {
        if (facts.empty()) return "";
        std::ostringstream oss;
        for (const auto& [k, v] : facts) {
            oss << k << ": " << v << "\n";
        }
        return oss.str();
    }

    /**
     * @brief Merge new facts into the existing facts store.
     *
     * New facts override existing ones for the same key.
     * Keys are never deleted — only overwritten.
     */
    void updateFacts(const std::unordered_map<std::string, std::string>& new_facts) {
        for (const auto& [k, v] : new_facts) {
            facts[k] = v;
        }
    }

    /**
     * @brief Limit facts to a maximum count by removing arbitrary entries.
     *
     * Called after updateFacts() to prevent unbounded growth.
     */
    void enforceFactsCeiling(size_t max_facts) {
        while (facts.size() > max_facts) {
            facts.erase(facts.begin());
        }
    }
};
