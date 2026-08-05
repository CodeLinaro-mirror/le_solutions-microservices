// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <chrono>
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
// ─────────────────────────────────────────────────────────────────────────────

struct ConversationSession {
    std::string session_id;
    std::string user_id;
    std::string current_model_id;

    // Shared OpenAI-format message history
    std::vector<json> messages;

    // Summarization state
    std::string summary_content;
    int summary_token_count = 0;
    int system_prompt_tokens = 0;
    std::string system_prompt_content;

    // Token tracking
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

    // Get messages suitable for sending to the model (strips private "_*" keys)
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
