// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/InternalDTOs.h"
#include "qai_forge/session/ConversationSession.h"
#include "qai_forge/backend/IGenerativeBackend.h"
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// IGenerativeMiddleware — Pluggable middleware for the generative pipeline
//
// Each middleware implements a single concern in the generative inference
// pipeline. The GenerativeOrchestrator builds a pipeline of middleware
// instances at startup based on BackendCapabilities.
//
// Current middleware implementations:
//   ExclusiveLockMiddleware    — one inference at a time (DSP/GPU lock)
//   ContextCompactionMiddleware — summarize when context window is full
//
// Design invariants:
//   - before() is called before inference. Returns false to abort the pipeline.
//   - after() is called after inference (even if before() returned false).
//   - Middleware is stateless — all state lives in GenerativeContext.
//   - The pipeline is built once at startup from BackendCapabilities.
//     Adding a new backend never requires changing existing middleware.
//
// See docs/unified-inference-service.md §6 for the full design rationale.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Context passed through the middleware pipeline.
 * Contains all state needed by any middleware.
 */
struct GenerativeContext {
    ConversationSession&               session;
    IGenerativeBackend&                backend;
    const CreateChatCompletionRequest& request;
};

/**
 * IGenerativeMiddleware — single-responsibility middleware interface.
 */
class IGenerativeMiddleware {
public:
    virtual ~IGenerativeMiddleware() = default;

    // Human-readable name for logging
    virtual std::string name() const = 0;

    /**
     * Called before inference.
     * @return true to continue the pipeline, false to abort.
     *         If false, after() is still called for cleanup.
     */
    virtual bool before(GenerativeContext& ctx) = 0;

    /**
     * Called after inference (always, even if before() returned false).
     * Used for cleanup: releasing locks, updating metrics, etc.
     */
    virtual void after(GenerativeContext& ctx) = 0;
};
