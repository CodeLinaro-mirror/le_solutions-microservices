// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/orchestration/IGenerativeOrchestrator.h"

#include <string>

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
    int estimateTokens(const json& messages, const json& tools) const;
    int applyContextEviction(json& messages,
                             const json& tools,
                             int max_context_length) const;
    json parseToolCalls(const std::string& text,
                        const std::string& delimiter) const;
    std::string extractPreToolContent(
        const std::string& text,
        const std::string& delimiter) const;

    static constexpr float kCompactionThreshold = 0.80f;
};
