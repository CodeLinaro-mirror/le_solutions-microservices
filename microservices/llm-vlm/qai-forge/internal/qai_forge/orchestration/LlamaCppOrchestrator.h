// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#ifdef QAI_FORGE_BUILD_LLAMACPP

#include "qai_forge/orchestration/IGenerativeOrchestrator.h"
#include "qai_forge/InternalDTOs.h"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <vector>

using json = nlohmann::ordered_json;

// ─────────────────────────────────────────────────────────────────────────────
// LlamaCppOrchestrator — concrete qai_forge::IGenerativeOrchestrator
// implementation for the direct-libllama LlamaCppBackend. Structurally
// identical to LiteRTLMOrchestrator: merges response history into messages,
// applies context eviction, delegates generation to
// LlamaCppBackend::generateStructured(), and reuses delimiter-based
// tool-call parsing (models embed a tool_call_delimiter in their chat
// template the same way litert-lm models do).
//
// No private per-turn memory (no postTurn() override — uses the public
// interface's default std::nullopt-returning implementation).
// ─────────────────────────────────────────────────────────────────────────────
class LlamaCppOrchestrator : public qai_forge::IGenerativeOrchestrator {
public:
    LlamaCppOrchestrator() = default;
    ~LlamaCppOrchestrator() override = default;

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

    json parseToolCalls(const std::string& text,
                        const std::string& delimiter) const;
    std::string extractPreToolContent(
        const std::string& text,
        const std::string& delimiter) const;
    std::vector<std::vector<uint8_t>> extractImagesFromMessages(
        json& messages) const;

    static constexpr float kCompactionThreshold = 0.70f;
};

#endif // QAI_FORGE_BUILD_LLAMACPP
