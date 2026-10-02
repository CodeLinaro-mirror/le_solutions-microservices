// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#ifdef QAI_FORGE_BUILD_GENIE

#include "qai_forge/orchestration/IGenerativeOrchestrator.h"
#include "qai_forge/InternalDTOs.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class ConversationSession;

// ─────────────────────────────────────────────────────────────────────────────
// GenieOrchestrator — Genie-specific generative orchestrator implementation
//
// Internal, qai-forge-owned implementation of the public
// qai_forge::IGenerativeOrchestrator interface for the Qualcomm GenIE SDK
// backend. This header stays internal — a third party implementing a new
// orchestrator only needs qai_forge/orchestration/IGenerativeOrchestrator.h
// (public) and never needs to see this class, exactly as Qwen25Adapter.h /
// GemmaAdapter.h (internal, concrete) relate to ModelAdapter.h (public,
// interface) in the adapters plugin pattern.
//
// Genie-specific responsibilities:
//   1. Universal post-inference KV reset via backend.resetKvAsync() — called
//      immediately after every generate() / generateVlm() so the reset runs
//      concurrently with returning the response to the HTTP layer.
//   2. Slot-based prompt assembly — system / summary / history slots with
//      proportional token ceilings scaled to the model's context window.
//   3. Genie-specific reasoning budget calculation via ReasoningBudgetCalculator.
//
// The request flows through a clean pipeline:
//   prepare()  — Step 1: extract/validate request
//               Step 2: rehydrate a transient ConversationSession from the
//                       public ConversationMemorySnapshot (summary/facts)
//               Step 3: prompt construction, reasoning budget, vision
//                       preprocessing — all stashed in PreparedState
//   execute()  — Step 4: run inference → ReasoningRouter → yield DTOs
//   postTurn() — Step 5: summarization / fact extraction / eviction
// ─────────────────────────────────────────────────────────────────────────────
class GenieOrchestrator : public qai_forge::IGenerativeOrchestrator {
public:
    GenieOrchestrator();

    std::unique_ptr<qai_forge::PreparedGenerationState> prepare(
        const CreateChatCompletionRequest& request,
        const qai_forge::ConversationMemorySnapshot& memory,
        const std::string& session_id) const override;

    StandardResponse execute(
        const qai_forge::GenerationJobInfo& info,
        qai_forge::PreparedGenerationState& prepared,
        qai_forge::IGenerativeBackend& backend,
        std::function<void(const StreamChunk&)> on_token) const override;

    std::optional<qai_forge::ConversationMemorySnapshot> postTurn(
        const qai_forge::GenerationJobInfo& info,
        qai_forge::PreparedGenerationState& prepared,
        const StandardResponse& response,
        qai_forge::IGenerativeBackend& backend) const override;

private:
    // Concrete prepared-state subclass — opaque to all scheduler and
    // OrchestratorAdapter code, which only ever hold a
    // unique_ptr<qai_forge::PreparedGenerationState>.
    struct PreparedState;

    GenieOrchestrator(const GenieOrchestrator&) = delete;
    GenieOrchestrator& operator=(const GenieOrchestrator&) = delete;

    void validateRequest(const CreateChatCompletionRequest& request) const;

    std::string buildContextPrompt(const ConversationSession& session,
                                   const CreateChatCompletionRequest& request,
                                   int thinking_budget = 0,
                                   int answer_budget = 0,
                                   bool has_tool_response = false) const;

    StandardResponse executeBlockingPrepared(
        const qai_forge::GenerationJobInfo& info,
        PreparedState& prepared,
        qai_forge::IGenerativeBackend& backend) const;

    StandardResponse executeStreamingPrepared(
        const qai_forge::GenerationJobInfo& info,
        PreparedState& prepared,
        qai_forge::IGenerativeBackend& backend,
        const std::function<void(const StreamChunk&)>& on_token) const;

    std::pair<std::string, int> generateSummary(
        const ConversationSession& session,
        const std::vector<json>& messages_to_summarize,
        const std::string& model_id,
        int max_summary_tokens,
        qai_forge::IGenerativeBackend& backend) const;

    void extractFacts(ConversationSession& session,
                      const std::vector<json>& messages_to_extract,
                      const std::string& model_id,
                      qai_forge::IGenerativeBackend& backend) const;

    void postTurnProcessing(ConversationSession& session,
                            const std::string& model_id,
                            qai_forge::IGenerativeBackend& backend) const;
};

#endif // QAI_FORGE_BUILD_GENIE
