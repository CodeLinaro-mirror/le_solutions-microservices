// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/backend/IGenerativeBackend.h"
#include <memory>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// BackendFactory — Selects the correct IGenerativeBackend based on runtime
//
// The "runtime" field in metadata.json is the routing key:
//   "genie"     → GenIEBackend   (Qualcomm GenIE SDK)
//   "litert_lm" → LiteRTLMBackend (future scheduler-owned support)
//   "onnxrt"    → OnnxRTBackend   (future)
//
// Scheduler path creates backend instances so each ModelRuntime can own its
// model handle and worker subprocess.
//
// Legacy path may still use singleton access until the scheduler-only cutover
// removes ChatOrchestratorImpl::handleBlocking/handleStreaming fallback.
//
// See docs/genai-backend-decoupling.md §8 for the full design rationale.
// ─────────────────────────────────────────────────────────────────────────────

class BackendFactory {
public:
    /**
     * Create a scheduler-owned backend instance for the given runtime.
     *
     * @param runtime  Runtime identifier from ModelConfig.runtime
     *                 ("genie", "litert_lm", "onnxrt")
     */
    static std::unique_ptr<IGenerativeBackend>
    createGenerativeBackend(const std::string& runtime);

    /**
     * Resolve a model id to its runtime and create a scheduler-owned backend.
     */
    static std::unique_ptr<IGenerativeBackend>
    createGenerativeBackendForModel(const std::string& model_id);

    /**
     * Legacy singleton access.
     *
     * Kept until the old ChatOrchestratorImpl fallback path is removed.
     */
    static IGenerativeBackend& getGenerativeBackend(const std::string& runtime);

    // BackendFactory is a pure static utility — no instances.
    BackendFactory() = delete;
};
