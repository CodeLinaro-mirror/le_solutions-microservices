// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/InternalDTOs.h"
#include <functional>
#include <variant>

// ─────────────────────────────────────────────────────────────────────────────
// ChatOrchestrator — Layer 2 Interface (called by Layer 1 controllers)
//
// This is the boundary contract between Layer 1 (Drogon HTTP) and Layer 2
// (Orchestration Pipeline). Layer 1 calls these methods and receives back
// transport-agnostic DTOs. Layer 1 is then responsible for formatting those
// DTOs into the OpenAI HTTP/SSE wire format.
//
// Layer 1 MUST NOT contain any inference logic.
// Layer 2 MUST NOT contain any FastAPI/Drogon/HTTP concepts.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Callback type for streaming responses.
 * Layer 1 provides this callback; Layer 2 calls it for each StreamChunk.
 * When finish_reason is set, the stream is complete.
 */
using StreamCallback = std::function<void(const StreamChunk& chunk)>;

/**
 * ChatOrchestrator is the Layer 2 interface.
 *
 * Concrete implementation (ChatOrchestratorImpl) will:
 *   1. Resolve or create a ConversationSession
 *   2. Run the request through the middleware pipeline
 *      (ToolMiddleware → SummarizationMiddleware → ConcurrencyMiddleware)
 *   3. Delegate to InferenceExecutor → ReasoningRouter
 *   4. Return DTOs to Layer 1
 */
class ChatOrchestrator {
public:
    virtual ~ChatOrchestrator() = default;

    /**
     * Handle a non-streaming chat completion request.
     *
     * @param request  Parsed request DTO from Layer 1
     * @return         StandardResponse DTO (Layer 1 formats this to JSON)
     * @throws         GenAIException on domain errors (Layer 1 maps to HTTP errors)
     */
    virtual StandardResponse handleBlocking(const CreateChatCompletionRequest& request) = 0;

    /**
     * Handle a streaming chat completion request.
     *
     * @param request   Parsed request DTO from Layer 1
     * @param callback  Layer 1 provides this; called for each StreamChunk
     * @throws          GenAIException on domain errors (Layer 1 maps to HTTP errors)
     */
    virtual void handleStreaming(const CreateChatCompletionRequest& request, StreamCallback callback) = 0;

    /**
     * Delete a session and clean up all associated resources.
     *
     * @param completion_id  The session/completion ID to delete
     * @return               true if deleted, false if not found
     */
    virtual bool deleteSession(const std::string& completion_id) = 0;

    /**
     * Cancel an actively executing request for a session.
     *
     * @param completion_id  The session/completion ID to cancel
     * @return               true if cancelled, false if not found or not active
     */
    virtual bool cancelSession(const std::string& completion_id) = 0;

    /**
     * Reset the KV cache for the currently loaded model.
     *
     * Called by the OIP /generate endpoint before and after each stateless
     * inference request to ensure clean state. This maps to IGenerativeBackend::resetKv()
     * on the active backend (e.g. Genie cache reset API).
     *
     * Safe to call even if no model is loaded (no-op in that case).
     *
     * @param model_id  Model to reset cache for (used to ensure correct backend)
     */
    virtual void resetKvCache(const std::string& model_id) = 0;

    /**
     * Factory method — returns the singleton orchestrator instance.
     * In production, this returns ChatOrchestratorImpl.
     */
    static ChatOrchestrator& getInstance();
};
