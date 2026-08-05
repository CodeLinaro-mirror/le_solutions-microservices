// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// BackendFactory — Runtime-based backend selection
//
// Routes the "runtime" field from metadata.json to the correct
// IGenerativeBackend implementation. Scheduler-owned execution creates a fresh
// backend instance per ModelRuntime so each resident model owns its worker.
//
// Current routing table:
//   "genie"     → owned GenIEBackend / legacy GenIEBackend singleton
//   "litert_lm" → legacy LiteRTLMBackend singleton only for now
//   "onnxrt"    → unsupported until implemented
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/backend/BackendFactory.h"
#include "qai_forge/InternalDTOs.h"
#include "qai_forge/backend/GenIEBackend.h"
#include "qai_forge/backend/LiteRTLMBackend.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/Logger.h"
#include <memory>

std::unique_ptr<IGenerativeBackend>
BackendFactory::createGenerativeBackend(const std::string& runtime) {
    if (runtime == "genie") {
        return std::make_unique<GenIEBackend>();
    }

    if (runtime == "litert_lm") {
        throw GenAIException(
            GenAIErrorCode::INVALID_REQUEST,
            "Scheduler-owned LiteRT-LM backend is not implemented yet.",
            400);
    }

    throw GenAIException(
        GenAIErrorCode::INVALID_REQUEST,
        "Unsupported generative backend runtime '" + runtime + "'.",
        400);
}

std::unique_ptr<IGenerativeBackend>
BackendFactory::createGenerativeBackendForModel(const std::string& model_id) {
    const auto* model_config =
        ModelConfigManager::getInstance().getModelConfig(model_id);
    if (!model_config) {
        throw GenAIException(
            GenAIErrorCode::MODEL_NOT_FOUND,
            "Model '" + model_id + "' not found. Check /v1/models for available models.",
            404);
    }

    return createGenerativeBackend(model_config->runtime);
}

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
