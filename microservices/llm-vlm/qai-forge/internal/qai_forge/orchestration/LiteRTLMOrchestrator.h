// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/orchestration/IGenerativeOrchestrator.h"
#include "qai_forge/backend/LiteRTLMBackend.h"

#include "minja/chat-template.hpp"

#include <memory>
#include <string>
#include <mutex>

// ─────────────────────────────────────────────────────────────────────────────
// LiteRTLMOrchestrator — concrete qai_forge::IGenerativeOrchestrator
// implementation for the LiteRT-LM backend.
//
// Internal, qai-forge-owned implementation — a third party implementing a
// new orchestrator only needs qai_forge/orchestration/IGenerativeOrchestrator.h
// (public) and never needs to see this class.
//
// LiteRT-LM has no private per-turn memory of its own (no postTurn()
// override — uses the public interface's default std::nullopt-returning
// implementation).
// ─────────────────────────────────────────────────────────────────────────────
class LiteRTLMOrchestrator : public qai_forge::IGenerativeOrchestrator {
public:
    LiteRTLMOrchestrator() = default;

    qai_forge::OrchestratorCapabilities capabilities() const override {
        return {false, false};
    }

    std::unique_ptr<qai_forge::PreparedGenerationState> prepare(
        const CreateChatCompletionRequest& request,
        const qai_forge::ConversationMemorySnapshot& memory,
        const std::string& session_id) const override;

    StandardResponse execute(
        const qai_forge::GenerationJobInfo& info,
        qai_forge::PreparedGenerationState& prepared,
        qai_forge::IGenerativeBackend& backend,
        std::function<void(const StreamChunk&)> on_token) const override;

private:
    // Concrete prepared-state subclass — opaque to all scheduler and
    // OrchestratorAdapter code, which only ever hold a
    // unique_ptr<qai_forge::PreparedGenerationState>.
    struct PreparedState;

    // ── Lazy metadata initialization ───────────────────────────────────────────
    bool initMetadataIfNeeded(qai_forge::IGenerativeBackend& backend) const;

    // ── Prompt rendering ───────────────────────────────────────────────────────
    // Renders the model's Jinja2 chat template (as provided by the LiteRT-LM
    // worker's METADATA message) via minja — a Jinja2 subset purpose-built
    // for HuggingFace chat templates. Falls back to a flat "role: content"
    // formatter if the template is empty or fails to compile/render.
    std::string renderPrompt(const json& messages,
                             const json& tools,
                             bool add_generation_prompt = true) const;

    // ── Mutable state for lazy init in const execute() ────────────────────────
    mutable std::string jinja_template_;
    // Compiled minja chat template — built once in initMetadataIfNeeded()
    // from jinja_template_. nullptr if the model's template is empty or
    // fails to compile (renderPrompt() falls back to a flat formatter).
    mutable std::unique_ptr<minja::chat_template> chat_template_;
    mutable std::string model_type_;
    mutable int         max_context_length_   = 0;
    mutable std::string think_start_;
    mutable std::string think_end_;
    mutable bool        metadata_initialized_ = false;
    mutable std::mutex  metadata_mutex_;

    static float compactionThreshold();
    static int   defaultContextLength();
};
