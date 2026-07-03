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
#include <nlohmann/json.hpp>

using json = nlohmann::ordered_json;

// ─────────────────────────────────────────────────────────────────────────────
// ConversationSession — Pure Data Record (Section 3.A of architecture design)
//
// This is a plain data object. It holds the shared message history and
// metadata for a single conversation session. It does NOT execute inference,
// manage handles, or contain business logic.
//
// Key design decisions:
//   - session_id is the stable chat completion ID returned to the client.
//   - messages is the shared OpenAI-format message history.
//   - DraftTurn pattern (Section 3.C): new messages are staged in a DraftTurn
//     before being committed to this session's history.
//   - Thinking content is stored with a private "_thinking_content" key and
//     stripped before being sent to the model (Section 3.F).
//   - Post-turn memory management (Phase 5):
//       evicted_message_count — number of oldest messages already summarized.
//       summary_content       — rolling summary of evicted messages (Slot 3).
//       facts                 — persistent key-value facts extracted from
//                               evicted messages (Slot 2).
// ─────────────────────────────────────────────────────────────────────────────

struct ConversationSession {
    std::string session_id;
    std::string user_id;
    std::string current_model_id;

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

    // ── Legacy fields (kept for compatibility) ────────────────────────────────
    int system_prompt_tokens = 0;
    std::string system_prompt_content;
    int total_cumulative_tokens = 0;

    // Timestamps
    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point last_activity;

    explicit ConversationSession(const std::string& id, const std::string& uid = "default_user")
        : session_id(id), user_id(uid),
          created_at(std::chrono::system_clock::now()),
          last_activity(std::chrono::system_clock::now()) {}

    // Add a message to history. Returns the index of the added message.
    size_t addMessage(const json& message) {
        messages.push_back(message);
        last_activity = std::chrono::system_clock::now();
        return messages.size() - 1;
    }

    /**
     * @brief Get messages suitable for sending to the model.
     *
     * Strips private "_*" keys (e.g. "_thinking_content").
     * Returns ALL messages (including evicted ones).
     * Use getHistoryMessages() for the history queue slot.
     */
    std::vector<json> getCleanMessages() const {
        std::vector<json> clean;
        clean.reserve(messages.size());
        for (const auto& msg : messages) {
            json clean_msg = json::object();
            for (const auto& [k, v] : msg.items()) {
                if (k.empty() || k[0] != '_') {
                    clean_msg[k] = v;
                }
            }
            clean.push_back(clean_msg);
        }
        return clean;
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
            std::string role = msg.value("role", "");
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
     * @brief Estimate total tokens in the active history queue.
     *
     * Counts tokens in messages[evicted_message_count..end], excluding system
     * messages. Uses a simple heuristic: 1 token ≈ 4 characters.
     */
    int estimateHistoryTokens() const {
        int total = 0;
        for (size_t i = evicted_message_count; i < messages.size(); ++i) {
            const auto& msg = messages[i];
            if (msg.value("role", "") == "system") continue;
            std::string content = msg.value("content", "");
            total += static_cast<int>(content.size() / 4) + 4;  // +4 per-message overhead
        }
        return total;
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

// ─────────────────────────────────────────────────────────────────────────────
// DraftTurn — Unit of Work for atomic session state updates (Section 3.C)
//
// New messages are staged here before inference. If inference succeeds,
// commit() atomically appends them to the session. If it fails, the draft
// is simply discarded — no messy rollback logic needed.
// ─────────────────────────────────────────────────────────────────────────────
struct DraftTurn {
    std::string session_id;
    std::string model_id;
    std::vector<json> new_messages;     // Messages to be committed on success
    std::vector<size_t> staged_indices; // Indices in session.messages after commit

    bool committed = false;

    explicit DraftTurn(const std::string& sid, const std::string& mid)
        : session_id(sid), model_id(mid) {}

    void addMessage(const json& msg) {
        new_messages.push_back(msg);
    }

    // Commit staged messages to the session. Returns indices of committed messages.
    std::vector<size_t> commit(ConversationSession& session) {
        staged_indices.clear();
        for (const auto& msg : new_messages) {
            staged_indices.push_back(session.addMessage(msg));
        }
        session.current_model_id = model_id;
        committed = true;
        return staged_indices;
    }
};
