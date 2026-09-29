// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#ifdef QAI_FORGE_BUILD_LLAMACPP

#include "qai_forge/orchestration/IGenerativeOrchestrator.h"
#include "qai_forge/InternalDTOs.h"
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::ordered_json;

namespace qai_forge {

// ─────────────────────────────────────────────────────────────────────────────
// LlamaCppOrchestrator — stateless orchestrator for the direct-libllama
// LlamaCppBackend. Structurally identical to LiteRTLMOrchestrator: merges
// response history into messages, applies context eviction, delegates
// generation to LlamaCppBackend::generateStructured(), and reuses the same
// delimiter-based tool-call parsing (models embed a tool_call_delimiter in
// their chat template the same way litert-lm models do).
// ─────────────────────────────────────────────────────────────────────────────
class LlamaCppOrchestrator : public IGenerativeOrchestrator {
public:
    LlamaCppOrchestrator() = default;
    ~LlamaCppOrchestrator() override = default;

    scheduler::GenerativeJobPtr createJob(
        scheduler::GenerativeJobContext context,
        scheduler::GenerativeCallbacks callbacks) const override;

    StandardResponse execute(
        scheduler::GenerativeJob& job,
        IGenerativeBackend& backend) const override;

private:
    json parseToolCalls(const std::string& text,
                        const std::string& delimiter) const;
    std::string extractPreToolContent(
        const std::string& text,
        const std::string& delimiter) const;
    std::vector<std::vector<uint8_t>> extractImagesFromMessages(
        json& messages) const;

    static constexpr float kCompactionThreshold = 0.70f;
};

} // namespace qai_forge

#endif // QAI_FORGE_BUILD_LLAMACPP
