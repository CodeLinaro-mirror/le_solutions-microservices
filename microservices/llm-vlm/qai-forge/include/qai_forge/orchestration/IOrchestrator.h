// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/InternalDTOs.h"
#include "qai_forge/backend/IGenerativeBackend.h"
#include <functional>
#include <nlohmann/json.hpp>

using json = nlohmann::ordered_json;

// ─────────────────────────────────────────────────────────────────────────────
// IOrchestrator — Stateless inference orchestration interface
//
// This is the runtime-agnostic interface that ModelRuntime uses to execute
// one inference turn. It replaces the ChatOrchestrator singleton pattern
// with a per-model-instance approach:
//
//   - One IOrchestrator instance is created per loaded model by BackendFactory.
//   - The orchestrator receives the backend as an injected parameter — it
//     never holds a singleton backend reference.
//   - Blocking vs streaming is controlled by the callback parameter:
//       callback == nullptr  → blocking (returns when inference is complete)
//       callback != nullptr  → streaming (tokens delivered via callback)
//
// Current implementations:
//   GenieOrchestrator — Qualcomm GenIE SDK (LLM + VLM)
//
// Future implementations:
//   LiteRTLMOrchestrator — LiteRT-LM (MediaPipe Tasks LlmInference)
//   OnnxRTOrchestrator   — ONNX Runtime
//
// Design invariants:
//   - IOrchestrator MUST NOT hold a reference to any backend singleton.
//   - IOrchestrator MUST NOT perform HTTP/transport operations.
//   - IOrchestrator MUST NOT register sessions in SessionManager.
//     (Session management for the scheduler path uses transient sessions
//      seeded from response_history; the HTTP server owns the session store.)
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Callback type for streaming token delivery.
 * Called once per StreamChunk as tokens arrive from the backend.
 * When finish_reason is set on the chunk, the stream is complete.
 *
 * Defined here (rather than in ChatOrchestrator.h) so that IOrchestrator
 * is self-contained and ChatOrchestrator.h can be removed in Phase 3.
 */
using OrchestratorStreamCallback = std::function<void(const StreamChunk& chunk)>;

class IOrchestrator {
public:
    virtual ~IOrchestrator() = default;

    /**
     * Execute one inference turn.
     *
     * Creates a transient ConversationSession seeded from response_history,
     * builds the context prompt, runs inference on the supplied backend, and
     * returns the result. The session is discarded after the call — the caller
     * (ResponseStore / HTTP layer) owns persistent state.
     *
     * @param request          Chat completion request (model, messages, params).
     * @param response_history Ancestor messages from ResponseStore (empty for
     *                         root responses or in-process embedding).
     * @param backend          The backend instance to run inference on.
     *                         Owned by ModelRuntime; must outlive this call.
     * @param callback         Stream callback. Pass nullptr for blocking mode.
     *                         In streaming mode, tokens are delivered via this
     *                         callback as they arrive; the return value still
     *                         contains the complete StandardResponse.
     * @param cancel           Cancellation predicate. Called periodically during
     *                         inference; return true to abort. Pass nullptr (or
     *                         an empty function) for non-cancellable requests.
     * @return                 StandardResponse with content, tool_calls, usage.
     *                         On cancellation, returns an empty StandardResponse.
     * @throws GenAIException  On inference failure or invalid request.
     */
    virtual StandardResponse execute(
        const CreateChatCompletionRequest& request,
        const json& response_history,
        IGenerativeBackend& backend,
        OrchestratorStreamCallback callback,
        std::function<bool()> cancel) = 0;
};
