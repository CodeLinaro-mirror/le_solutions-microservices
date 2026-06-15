// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// BackendFactory — Runtime-based backend selection
//
// Routes the "runtime" field from metadata.json to the correct
// IGenerativeBackend singleton. This is the single place that needs to change
// when a new generative backend is added — the orchestrator is never modified.
//
// Current routing table:
//   "genie"     → GenIEBackend   (Qualcomm GenIE SDK — LLM + VLM)
//   "litert_lm" → GenIEBackend   (placeholder until LiteRTLMBackend is implemented)
//   "onnxrt"    → GenIEBackend   (placeholder until OnnxRTBackend is implemented)
//   <unknown>   → GenIEBackend   (safe default)
//
// When LiteRTLMBackend is implemented (Phase 3):
//   Replace the "litert_lm" case with LiteRTLMBackend::getInstance()
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/backend/BackendFactory.h"
#include "qai_forge/backend/GenIEBackend.h"
#include "qai_forge/backend/LiteRTLMBackend.h"
#include "qai_forge/utils/Logger.h"

IGenerativeBackend& BackendFactory::getGenerativeBackend(const std::string& runtime) {
    if (runtime == "genie") {
        return GenIEBackend::getInstance();
    }

    if (runtime == "litert_lm") {
        return LiteRTLMBackend::getInstance();
    }

    // ── Future backends ───────────────────────────────────────────────────────
    // if (runtime == "onnxrt") {
    //     return OnnxRTBackend::getInstance();
    // }

    // Safe default: fall back to GenIEBackend for unknown runtime values.
    LOG_WARN("[BackendFactory] Unknown runtime '" << runtime
             << "' — falling back to GenIEBackend");
    return GenIEBackend::getInstance();
}
