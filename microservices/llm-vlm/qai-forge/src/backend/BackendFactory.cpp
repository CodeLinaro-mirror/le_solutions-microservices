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
#include "qai_forge/backend/LiteRTBackend.h"
#include "qai_forge/backend/LiteRTLMBackend.h"
#include "qai_forge/backend/QNNBackend.h"
#include "qai_forge/backend/SNPEBackend.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/orchestration/GenieOrchestrator.h"
#include "qai_forge/orchestration/LiteRTLMOrchestrator.h"
#include "qai_forge/utils/Logger.h"
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

RuntimePair BackendFactory::createRuntimePair(const std::string& model_id) {
    RuntimePair pair;
    pair.backend = createGenerativeBackendForModel(model_id);

    // Select the correct orchestrator based on the backend runtime
    const auto* model_config =
        ModelConfigManager::getInstance().getModelConfig(model_id);
    std::string runtime = model_config ? model_config->runtime : "genie";

    if (runtime == "litert_lm") {
        // Phase 4: LiteRTLMOrchestrator
        // Do NOT call loadModel() here — ModelRuntime::executorLoop() is responsible
        // for calling backend_->loadModel() when it transitions to the Loading state.
        // Calling it here would cause a double-load (worker started twice).
        // LiteRTLMOrchestrator initializes its metadata lazily on the first execute()
        // call, after ModelRuntime has already loaded the model and the worker has
        // sent the METADATA IPC event.
        auto* litert_backend = dynamic_cast<LiteRTLMBackend*>(pair.backend.get());
        if (litert_backend) {
            pair.orchestrator = std::make_unique<LiteRTLMOrchestrator>();
        } else {
            LOG_WARN("[BackendFactory] litert_lm runtime but backend is not LiteRTLMBackend"
                     " — falling back to GenieOrchestrator");
            pair.orchestrator = std::make_unique<GenieOrchestrator>();
        }
    } else {
        pair.orchestrator = std::make_unique<GenieOrchestrator>();
    }

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
