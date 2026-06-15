// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/backend/IGenerativeBackend.h"
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// BackendFactory — Selects the correct IGenerativeBackend based on runtime
//
// The "runtime" field in metadata.json is the routing key:
//   "genie"     → GenIEBackend::getInstance()   (current — Qualcomm GenIE SDK)
//   "litert_lm" → LiteRTLMBackend::getInstance() (future — MediaPipe Tasks)
//   "onnxrt"    → OnnxRTBackend::getInstance()   (future — ONNX Runtime)
//
// All backends are singletons. BackendFactory returns a reference to the
// appropriate singleton based on the runtime string.
//
// Design invariant: ChatOrchestratorImpl uses BackendFactory to select the
// backend at construction time. When a new backend is added, only
// BackendFactory::getGenerativeBackend() needs to change — the orchestrator
// is never modified.
//
// See docs/genai-backend-decoupling.md §8 for the full design rationale.
// ─────────────────────────────────────────────────────────────────────────────

class BackendFactory {
public:
    /**
     * Returns the IGenerativeBackend singleton for the given runtime identifier.
     *
     * @param runtime  Runtime identifier from ModelConfig.runtime
     *                 ("genie", "litert_lm", "onnxrt")
     * @return         Reference to the appropriate backend singleton.
     *                 Falls back to GenIEBackend for unknown runtime values.
     */
    static IGenerativeBackend& getGenerativeBackend(const std::string& runtime);

    // BackendFactory is a pure static utility — no instances.
    BackendFactory() = delete;
};
