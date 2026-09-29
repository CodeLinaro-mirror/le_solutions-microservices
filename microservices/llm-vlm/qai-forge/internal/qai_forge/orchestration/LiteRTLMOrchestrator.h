// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/orchestration/IGenerativeOrchestrator.h"
#include "qai_forge/backend/LiteRTLMBackend.h"

#include "minja/chat-template.hpp"

#include <memory>
#include <string>
#include <mutex>

class LiteRTLMOrchestrator : public IGenerativeOrchestrator {
public:
    LiteRTLMOrchestrator() = default;

    scheduler::GenerativeJobPtr createJob(
        scheduler::GenerativeJobContext context,
        scheduler::GenerativeCallbacks callbacks) const override;

    StandardResponse execute(
        scheduler::GenerativeJob& job,
        IGenerativeBackend& backend) const override;

private:
    // ── Lazy metadata initialization ───────────────────────────────────────────
    bool initMetadataIfNeeded(IGenerativeBackend& backend) const;

    // ── Prompt rendering ───────────────────────────────────────────────────────
    // Renders the model's Jinja2 chat template (as provided by the LiteRT-LM
    // worker's METADATA message) via minja — a Jinja2 subset purpose-built
    // for HuggingFace chat templates. Falls back to a flat "role: content"
    // formatter if the template is empty or fails to compile/render.
    std::string renderPrompt(const json& messages,
                             const json& tools,
                             bool add_generation_prompt = true) const;

    // ── Context management ─────────────────────────────────────────────────────
    int estimateTokens(const json& messages, const json& tools) const;
    int applyContextEviction(json& messages,
                             const json& tools,
                             int max_context_length) const;

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
