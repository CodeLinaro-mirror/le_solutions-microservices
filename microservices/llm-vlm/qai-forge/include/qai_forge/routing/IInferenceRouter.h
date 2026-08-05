// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/orchestration/ChatOrchestrator.h"
#include "qai_forge/dto/TensorDTOs.h"
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// IInferenceRouter — Layer 2 routing interface
//
// Routes incoming requests to the correct orchestrator based on model_type:
//   "generative"   → ChatOrchestrator (LLM/VLM — existing pipeline)
//   "conventional" → ConventionalAIOrchestrator (classification/detection)
//
// The transport layer (Layer 1) calls this interface instead of calling
// ChatOrchestrator directly. This allows the same transport to serve both
// generative and conventional AI models.
//
// Design invariant: Layer 1 controllers NEVER branch on model type.
// They call IInferenceRouter and let it dispatch to the correct orchestrator.
//
// See docs/unified-inference-service.md §3 for the full design rationale.
// ─────────────────────────────────────────────────────────────────────────────

class IInferenceRouter {
public:
    virtual ~IInferenceRouter() = default;

    // ── Generative AI path (existing — unchanged) ─────────────────────────────

    /**
     * Handle a non-streaming generative AI request.
     * Routes to ChatOrchestrator::handleBlocking() for "generative" models.
     * Throws GenAIException for "conventional" models (wrong endpoint).
     */
    virtual StandardResponse handleBlocking(
        const CreateChatCompletionRequest& request) = 0;

    /**
     * Handle a streaming generative AI request.
     * Routes to ChatOrchestrator::handleStreaming() for "generative" models.
     * Throws GenAIException for "conventional" models (wrong endpoint).
     */
    virtual void handleStreaming(
        const CreateChatCompletionRequest& request,
        StreamCallback callback) = 0;

    /**
     * Delete a generative AI session.
     */
    virtual bool deleteSession(const std::string& completion_id) = 0;

    /**
     * Cancel an active generative AI request.
     */
    virtual bool cancelSession(const std::string& completion_id) = 0;

    // ── Conventional AI path (new) ────────────────────────────────────────────

    /**
     * Handle a conventional AI tensor inference request.
     * Routes to ConventionalAIOrchestrator for "conventional" models.
     * Returns a 501 Not Implemented response for "generative" models
     * (wrong endpoint — use /v1/chat/completions instead).
     */
    virtual TensorInferenceResponse handleInfer(
        const TensorInferenceRequest& request) = 0;

    // ── Factory ───────────────────────────────────────────────────────────────

    /**
     * Returns the singleton InferenceRouter instance.
     */
    static IInferenceRouter& getInstance();
};
