// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// PredictiveAIOrchestrator — Layer 2 orchestrator for Predictive AI
//
// Routes tensor inference requests to QNNBackend or SNPEBackend based on
// the model's "runtime" field in metadata.json.
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/orchestration/PredictiveAIOrchestrator.h"
#include "qai_forge/backend/LiteRTBackend.h"
#include "qai_forge/backend/QNNBackend.h"
#include "qai_forge/backend/SNPEBackend.h"
#include "qai_forge/backend/IInferenceBackend.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/InternalDTOs.h"
#include "qai_forge/utils/Logger.h"
#include <stdexcept>

PredictiveAIOrchestrator& PredictiveAIOrchestrator::getInstance() {
    static PredictiveAIOrchestrator instance;
    return instance;
}

TensorInferenceResponse PredictiveAIOrchestrator::handleInfer(
    const TensorInferenceRequest& request)
{
    auto& cfg = ModelConfigManager::getInstance();

    // Validate model exists
    if (!cfg.validateModel(request.model)) {
        throw GenAIException(
            GenAIErrorCode::MODEL_NOT_FOUND,
            "Model '" + request.model + "' not found. Check /v1/models for available models.",
            404);
    }

    // Validate model type
    std::string model_type = cfg.getModelType(request.model);
    if (model_type != "predictive") {
        throw GenAIException(
            GenAIErrorCode::INVALID_REQUEST,
            "Model '" + request.model + "' is a generative model. "
            "Use POST /v1/chat/completions instead of /v2/models/{model}/infer.",
            400);
    }

    // Select backend based on runtime
    std::string runtime = cfg.getRuntime(request.model);
    IInferenceBackend* backend = nullptr;

    if (runtime == "qnn") {
        backend = &QNNBackend::getInstance();
    } else if (runtime == "snpe") {
        backend = &SNPEBackend::getInstance();
    } else if (runtime == "litert") {
        backend = &LiteRTBackend::getInstance();
    } else {
        throw GenAIException(
            GenAIErrorCode::INVALID_REQUEST,
            "Unknown Predictive AI runtime '" + runtime + "' for model '" + request.model + "'.",
            400);
    }

    // Initialize backend if needed (lazy initialization)
    if (!backend->isHealthy() ||
        (runtime == "qnn"    && !QNNBackend::getInstance().isHealthy())  ||
        (runtime == "snpe"   && !SNPEBackend::getInstance().isHealthy()) ||
        (runtime == "litert" && !LiteRTBackend::getInstance().isHealthy()))
    {
        backend->initialize(request.model, "");
    }

    LOG_DEBUG("[PredictiveAIOrchestrator] Routing model='" << request.model
              << "' runtime='" << runtime << "' to " << backend->name());

    // Run inference
    try {
        return backend->infer(request);
    } catch (const std::exception& e) {
        throw GenAIException(
            GenAIErrorCode::INFERENCE_FAILED,
            std::string("Inference failed for model '") + request.model + "': " + e.what(),
            500);
    }
}
