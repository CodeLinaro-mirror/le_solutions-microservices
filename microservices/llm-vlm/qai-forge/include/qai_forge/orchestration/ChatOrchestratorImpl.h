// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/orchestration/ChatOrchestrator.h"
#include "qai_forge/session/SessionManager.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/backend/IGenerativeBackend.h"
#include <functional>
#include <memory>
#include <utility>

// ─────────────────────────────────────────────────────────────────────────────
// ChatOrchestratorImpl — Layer 2 Pipeline Implementation
//
// Implements the ChatOrchestrator interface. This is the "God Class" fix
// described in Section 3.D of architecture_refactoring_design.md.
//
// The request flows through a clean pipeline:
//   Step 1: Extractor     — parse messages and config from the request DTO
//   Step 2: Session Resolver — find/create ConversationSession, issue DraftTurn
//   Step 3: Context prep   — summarization and prompt construction
//   Step 4: Execution      — injected backend → ReasoningRouter → yield DTOs
//
// Layer 1 (ChatController) calls handleBlocking() or handleStreaming() and
// receives transport-agnostic DTOs. It never sees inference logic.
// ─────────────────────────────────────────────────────────────────────────────
class ChatOrchestratorImpl : public ChatOrchestrator {
public:
    using CancellationPredicate = std::function<bool()>;

    static ChatOrchestratorImpl& getInstance();

    // ── ChatOrchestrator interface ─────────────────────────────────────────────
    StandardResponse handleBlocking(const CreateChatCompletionRequest& request) override;
    void handleStreaming(const CreateChatCompletionRequest& request, StreamCallback callback) override;
    bool deleteSession(const std::string& completion_id) override;
    bool cancelSession(const std::string& completion_id) override;

    /**
     * @brief Execute a blocking chat request using the supplied backend.
     * @detail Scheduler-safe entry point: no global concurrency guard is taken
     *         and no singleton backend is used.
     */
    StandardResponse executeBlocking(
        const CreateChatCompletionRequest& request,
        IGenerativeBackend& backend,
        CancellationPredicate cancel_requested = {});

    /**
     * @brief Execute a streaming chat request using the supplied backend.
     * @detail Scheduler-safe entry point: no global concurrency guard is taken
     *         and no singleton backend is used.
     */
    void executeStreaming(
        const CreateChatCompletionRequest& request,
        IGenerativeBackend& backend,
        StreamCallback callback,
        CancellationPredicate cancel_requested = {});

private:
    // Constructor initializes the legacy fallback backend reference.
    ChatOrchestratorImpl();
    ChatOrchestratorImpl(const ChatOrchestratorImpl&) = delete;
    ChatOrchestratorImpl& operator=(const ChatOrchestratorImpl&) = delete;

    // ── Active generative backend ──────────────────────────────────────────────
    // Legacy fallback backend. Scheduler-safe execution receives an injected
    // backend instance through executeBlocking()/executeStreaming().
    // The orchestrator calls only IGenerativeBackend methods — it never touches
    // InferenceWorkerManager or VlmInferenceWorkerManager directly.
    IGenerativeBackend& backend_;

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
     * Build the compacted context prompt for the inference worker.
     * Assembles: [system_prompt] + [summary] + [post-summary messages] + [latest user message]
     * This is the Context Compaction described in Section 6 of the design.
     */
    std::string buildContextPrompt(const ConversationSession& session,
                                   const CreateChatCompletionRequest& request) const;

    StandardResponse executeBlockingPrepared(
        const CreateChatCompletionRequest& request,
        std::shared_ptr<ConversationSession> session,
        DraftTurn&& draft,
        IGenerativeBackend& backend,
        const CancellationPredicate& cancel_requested);

    void executeStreamingPrepared(
        const CreateChatCompletionRequest& request,
        std::shared_ptr<ConversationSession> session,
        DraftTurn&& draft,
        IGenerativeBackend& backend,
        StreamCallback callback,
        const CancellationPredicate& cancel_requested);
};
