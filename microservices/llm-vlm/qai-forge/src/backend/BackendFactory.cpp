// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// BackendFactory — Runtime-based backend selection
//
// Routes the "runtime" field from metadata.json to the correct
// qai_forge::IGenerativeBackend/qai_forge::IInferenceBackend implementation. All routing now
// delegates to PluginRegistry, which is populated at process startup
// by PluginLoader (dlopen-based discovery of qaiforge_plugin_*.so files) —
// BackendFactory itself no longer needs to know which concrete backend
// types exist or maintain a compile-time #ifdef ladder per runtime.
//
// Scheduler-owned execution creates a fresh backend instance per
// ModelRuntime/PredictiveModelRuntime so each resident model owns its own
// worker/session state.
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/backend/BackendFactory.h"
#include "qai_forge/InternalDTOs.h"
#include "qai_forge/plugin/PluginRegistry.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/orchestration/PredictiveOrchestrator.h"
#include "qai_forge/utils/Logger.h"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <vector>

std::unique_ptr<BackendAdapter>
BackendFactory::createGenerativeBackend(const std::string& runtime) {
    auto backend =
        PluginRegistry::instance().createGenerativeBackend(runtime);
    if (!backend) {
        throw GenAIException(
            GenAIErrorCode::INVALID_REQUEST,
            "Unsupported generative backend runtime '" + runtime + "'.",
            400);
    }
    return backend;
}

std::unique_ptr<BackendAdapter>
BackendFactory::createGenerativeBackendForModel(const std::string& model_id) {
    const auto model_config =
        ModelConfigManager::getInstance().getModelConfig(model_id);
    if (!model_config) {
        throw GenAIException(
            GenAIErrorCode::MODEL_NOT_FOUND,
            "Model '" + model_id + "' not found. Check /v1/models for available models.",
            404);
    }

    return createGenerativeBackend(model_config->runtime);
}

std::shared_ptr<OrchestratorAdapter>
BackendFactory::createGenerativeOrchestrator(const std::string& runtime) {
    auto orchestrator =
        PluginRegistry::instance().createGenerativeOrchestrator(runtime);
    if (!orchestrator) {
        throw GenAIException(
            GenAIErrorCode::INVALID_REQUEST,
            "Unsupported generative orchestrator runtime '" + runtime + "'.",
            400);
    }
    return orchestrator;
}

std::shared_ptr<OrchestratorAdapter>
BackendFactory::createGenerativeOrchestratorForModel(
    const std::string& model_id) {
    const auto model_config =
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
    const auto model_config =
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

// ─────────────────────────────────────────────────────────────────────────────
// Predictive AI backends
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<PredictiveBackendAdapter>
BackendFactory::createPredictiveBackend(const std::string& runtime) {
    auto backend =
        PluginRegistry::instance().createPredictiveBackend(runtime);
    if (!backend) {
        throw GenAIException(
            GenAIErrorCode::INVALID_REQUEST,
            "Unknown predictive AI runtime '" + runtime + "'. "
            "Supported runtimes: qnn, qnn_context_binary, snpe, qnn_dlc, litert, tflite.",
            400);
    }
    return backend;
}

std::unique_ptr<PredictiveBackendAdapter>
BackendFactory::createPredictiveBackendForModel(const std::string& model_id) {
    const auto model_config =
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

// ─────────────────────────────────────────────────────────────────────────────
// Capability discovery
// ─────────────────────────────────────────────────────────────────────────────

nlohmann::ordered_json BackendFactory::supportedRuntimes() {
    // Reflects the PluginRegistry's actual current contents — i.e.
    // what plugin .so's were successfully dlopen'd and registered at
    // startup, not a compile-time macro echo. Sorted for stable output.
    auto& registry = PluginRegistry::instance();

    std::vector<std::string> generative_runtimes =
        registry.registeredGenerativeRuntimes();
    std::vector<std::string> predictive_runtimes =
        registry.registeredPredictiveRuntimes();
    std::sort(generative_runtimes.begin(), generative_runtimes.end());
    std::sort(predictive_runtimes.begin(), predictive_runtimes.end());

    nlohmann::ordered_json generative = nlohmann::ordered_json::array();
    for (const auto& runtime : generative_runtimes) {
        generative.push_back(runtime);
    }

    nlohmann::ordered_json predictive = nlohmann::ordered_json::array();
    for (const auto& runtime : predictive_runtimes) {
        predictive.push_back(runtime);
    }

    nlohmann::ordered_json result;
    result["generative"] = std::move(generative);
    result["predictive"] = std::move(predictive);
    return result;
}
