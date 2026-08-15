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
//   "genie"     → GenIEBackend + GenieOrchestrator (scheduler-owned)
//   "litert_lm" → LiteRTLMBackend + LiteRTLMOrchestrator (scheduler-owned)
//   "llamacpp"  → LlamaCppBackend + LlamaCppOrchestrator (scheduler-owned, requires BUILD_LLAMACPP=ON)
//   "onnxrt"    → unsupported until implemented
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/backend/BackendFactory.h"
#include "qai_forge/InternalDTOs.h"
#include "qai_forge/backend/GenIEBackend.h"
#include "qai_forge/backend/LiteRTBackend.h"
#include "qai_forge/backend/LiteRTLMBackend.h"
#include "qai_forge/backend/QNNBackend.h"
#include "qai_forge/backend/SNPEBackend.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/orchestration/GenieOrchestrator.h"
#include "qai_forge/orchestration/LiteRTLMOrchestrator.h"
#include "qai_forge/orchestration/PredictiveOrchestrator.h"
#include "qai_forge/utils/Logger.h"

#ifdef QAI_FORGE_BUILD_LLAMACPP
#include "qai_forge/backend/LlamaCppBackend.h"
#include "qai_forge/orchestration/LlamaCppOrchestrator.h"
#endif
// PredictiveWorkerManager must be a complete type here because
// std::make_unique<QNNBackend/SNPEBackend/LiteRTBackend>() instantiates
// std::default_delete<T>, which calls ~T(), which destroys the
// std::unique_ptr<PredictiveWorkerManager> member inside each backend.
// The forward declaration in the backend headers is not sufficient.
#include "qai_forge/worker/PredictiveWorkerManager.h"
#include <memory>

std::unique_ptr<IGenerativeBackend>
BackendFactory::createGenerativeBackend(const std::string& runtime) {
    if (runtime == "genie") {
        return std::make_unique<GenIEBackend>();
    }

    if (runtime == "litert_lm") {
        // Phase 3: Scheduler-owned LiteRTLMBackend instance
        return std::make_unique<LiteRTLMBackend>();
    }

#ifdef QAI_FORGE_BUILD_LLAMACPP
    if (runtime == "llamacpp") {
        // Phase 4: Scheduler-owned LlamaCppBackend instance
        return std::make_unique<qai_forge::LlamaCppBackend>("/usr/bin/llama-server");
    }
#endif

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

std::shared_ptr<IGenerativeOrchestrator>
BackendFactory::createGenerativeOrchestrator(const std::string& runtime) {
    if (runtime == "genie") {
        return std::make_shared<GenieOrchestrator>();
    }

    if (runtime == "litert_lm") {
        return std::make_shared<LiteRTLMOrchestrator>();
    }

#ifdef QAI_FORGE_BUILD_LLAMACPP
    if (runtime == "llamacpp") {
        return std::make_shared<qai_forge::LlamaCppOrchestrator>();
    }
#endif

    throw GenAIException(
        GenAIErrorCode::INVALID_REQUEST,
        "Unsupported generative orchestrator runtime '" + runtime + "'.",
        400);
}

std::shared_ptr<IGenerativeOrchestrator>
BackendFactory::createGenerativeOrchestratorForModel(
    const std::string& model_id) {
    const auto* model_config =
        ModelConfigManager::getInstance().getModelConfig(model_id);
    if (!model_config) {
        throw GenAIException(
            GenAIErrorCode::MODEL_NOT_FOUND,
            "Model '" + model_id +
                "' not found. Check /v1/models for available models.",
            404);
    }

    return createGenerativeOrchestrator(model_config->runtime);
}

RuntimePair BackendFactory::createRuntimePair(const std::string& model_id) {
    const auto* model_config =
        ModelConfigManager::getInstance().getModelConfig(model_id);
    if (!model_config) {
        throw GenAIException(
            GenAIErrorCode::MODEL_NOT_FOUND,
            "Model '" + model_id +
                "' not found. Check /v1/models for available models.",
            404);
    }

    RuntimePair pair;
    pair.backend = createGenerativeBackend(model_config->runtime);
    pair.orchestrator = createGenerativeOrchestrator(model_config->runtime);
    return pair;
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

// ─────────────────────────────────────────────────────────────────────────────
// Predictive AI backends
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<IInferenceBackend>
BackendFactory::createPredictiveBackend(const std::string& runtime) {
    // QNN context binary (HTP/GPU/CPU)
    if (runtime == "qnn" || runtime == "qnn_context_binary") {
        return std::make_unique<QNNBackend>();
    }

    // SNPE DLC container (DSP/GPU/AIP/CPU)
    if (runtime == "snpe" || runtime == "qnn_dlc") {
        return std::make_unique<SNPEBackend>();
    }

    // LiteRT / TFLite (NPU dispatch + CPU fallback)
    if (runtime == "litert" || runtime == "tflite") {
        return std::make_unique<LiteRTBackend>();
    }

    throw GenAIException(
        GenAIErrorCode::INVALID_REQUEST,
        "Unknown predictive AI runtime '" + runtime + "'. "
        "Supported runtimes: qnn, qnn_context_binary, snpe, qnn_dlc, litert, tflite.",
        400);
}

std::unique_ptr<IInferenceBackend>
BackendFactory::createPredictiveBackendForModel(const std::string& model_id) {
    const auto* model_config =
        ModelConfigManager::getInstance().getModelConfig(model_id);
    if (!model_config) {
        throw GenAIException(
            GenAIErrorCode::MODEL_NOT_FOUND,
            "Model '" + model_id + "' not found. Check /v1/models for available models.",
            404);
    }

    return createPredictiveBackend(model_config->runtime);
}

std::shared_ptr<PredictiveOrchestrator>
BackendFactory::createPredictiveOrchestrator() {
    static const auto orchestrator =
        std::make_shared<PredictiveOrchestrator>();
    return orchestrator;
}
