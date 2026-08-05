// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/orchestration/ChatOrchestrator.h"
#include "qai_forge/orchestration/IGenerativeMiddleware.h"
#include "qai_forge/session/SessionManager.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/backend/IGenerativeBackend.h"
#include "qai_forge/backend/BackendFactory.h"
#include <memory>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// ChatOrchestratorImpl — Layer 2 Pipeline Implementation
//
// Implements the ChatOrchestrator interface. This is the "God Class" fix
// described in Section 3.D of architecture_refactoring_design.md.
//
// The request flows through a clean pipeline:
//   Step 1: Extractor     — parse messages and config from the request DTO
//   Step 2: Session Resolver — find/create ConversationSession, issue DraftTurn
//   Step 3: Middleware    — ToolMiddleware, SummarizationMiddleware, ConcurrencyMiddleware
//   Step 4: Execution     — InferenceExecutor → ReasoningRouter → yield DTOs
//
// Layer 1 (ChatController) calls handleBlocking() or handleStreaming() and
// receives transport-agnostic DTOs. It never sees inference logic.
// ─────────────────────────────────────────────────────────────────────────────
class ChatOrchestratorImpl : public ChatOrchestrator {
public:
    static ChatOrchestratorImpl& getInstance();

    // ── ChatOrchestrator interface ─────────────────────────────────────────────
    StandardResponse handleBlocking(const CreateChatCompletionRequest& request) override;
    void handleStreaming(const CreateChatCompletionRequest& request, StreamCallback callback) override;
    bool deleteSession(const std::string& completion_id) override;
    bool cancelSession(const std::string& completion_id) override;

private:
    // Constructor initializes backend_ reference to GenIEBackend singleton.
    // Future: BackendFactory::getBackend(runtime) will select based on model's
    // "runtime" field in metadata.json.
    ChatOrchestratorImpl();
    ChatOrchestratorImpl(const ChatOrchestratorImpl&) = delete;
    ChatOrchestratorImpl& operator=(const ChatOrchestratorImpl&) = delete;

    // ── Active generative backend ──────────────────────────────────────────────
    // Selected by BackendFactory based on the default model's "runtime" field.
    // The orchestrator calls only IGenerativeBackend methods — it never touches
    // InferenceWorkerManager or VlmInferenceWorkerManager directly.
    IGenerativeBackend& backend_;

    // ── Middleware pipeline ────────────────────────────────────────────────────
    // Built at construction time from backend_.capabilities().
    // Replaces the hardcoded ConcurrencyMiddleware::Guard and
    // SummarizationMiddleware::checkAndSummarize() calls.
    std::vector<std::unique_ptr<IGenerativeMiddleware>> middleware_pipeline_;

    // ── Pipeline steps ─────────────────────────────────────────────────────────

    /**
     * Step 1: Extract and validate the request.
     * Throws GenAIException(INVALID_REQUEST) if model is not found.
     */
    void validateRequest(const CreateChatCompletionRequest& request);

    /**
     * Step 2: Resolve or create a ConversationSession and issue a DraftTurn.
     * The session_id is derived from the request (user field or hash-based lookup).
     */
    std::pair<std::shared_ptr<ConversationSession>, DraftTurn>
    resolveSessionAndDraft(const CreateChatCompletionRequest& request);

    /**
     * Step 3a: ToolMiddleware — detect if this is a tool continuation.
     * Returns true if the last message is a tool response and there is an
     * active tool call waiting for it.
     */
    bool isToolContinuation(const ConversationSession& session,
                             const CreateChatCompletionRequest& request) const;

    /**
     * Build the middleware pipeline from backend capabilities.
     * Called once in the constructor. The pipeline is fixed for the lifetime
     * of the orchestrator instance.
     */
    void buildMiddlewarePipeline();

    /**
     * Build the compacted context prompt for the inference worker.
     * Assembles: [system_prompt] + [summary] + [post-summary messages] + [latest user message]
     * This is the Context Compaction described in Section 6 of the design.
     */
    std::string buildContextPrompt(const ConversationSession& session,
                                   const CreateChatCompletionRequest& request) const;
};
