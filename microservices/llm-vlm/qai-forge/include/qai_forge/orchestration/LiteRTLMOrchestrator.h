// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/orchestration/IGenerativeOrchestrator.h"
#include "qai_forge/backend/LiteRTLMBackend.h"

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
    std::string renderPrompt(const json& messages,
                             const json& tools,
                             bool add_generation_prompt = true) const;
    std::string renderJinja(const std::string& tmpl,
                            const json& context) const;

    // ── Context management ─────────────────────────────────────────────────────
    int estimateTokens(const json& messages, const json& tools) const;
    int applyContextEviction(json& messages,
                             const json& tools,
                             int max_context_length) const;

    // ── Tool call parsing ──────────────────────────────────────────────────────
    json parseToolCalls(const std::string& text,
                        const std::string& delimiter) const;
    std::string extractPreToolContent(const std::string& text,
                                      const std::string& delimiter) const;

    // ── Mutable state for lazy init in const execute() ────────────────────────
    mutable std::string jinja_template_;
    mutable std::string model_type_;
    mutable int         max_context_length_   = 0;
    mutable std::string tool_call_delimiter_;
    mutable std::string tool_response_delimiter_;
    mutable std::string think_start_;
    mutable std::string think_end_;
    mutable bool        metadata_initialized_ = false;
    mutable std::mutex  metadata_mutex_;

    static float compactionThreshold();
    static int   defaultContextLength();
};
