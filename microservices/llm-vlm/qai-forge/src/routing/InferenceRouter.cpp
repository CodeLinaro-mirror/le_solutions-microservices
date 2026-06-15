// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// InferenceRouter — Routes requests to the correct orchestrator
//
// Routes based on model_type from ModelConfigManager:
//   "generative"   → ChatOrchestrator (existing pipeline — unchanged)
//   "conventional" → ConventionalAIOrchestrator (Phase 4 — stub for now)
//
// Design invariant: Layer 1 controllers call IInferenceRouter::getInstance()
// instead of ChatOrchestrator::getInstance() directly. This allows the same
// transport to serve both generative and conventional AI models without any
// branching in the transport layer.
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/routing/IInferenceRouter.h"
#include "qai_forge/orchestration/ChatOrchestrator.h"
#include "qai_forge/orchestration/ConventionalAIOrchestrator.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/Logger.h"

// ─────────────────────────────────────────────────────────────────────────────
// InferenceRouter — Concrete implementation
// ─────────────────────────────────────────────────────────────────────────────

class InferenceRouter : public IInferenceRouter {
public:
    static InferenceRouter& getInstance() {
        static InferenceRouter instance;
        return instance;
    }

    // ── Generative AI path — delegates to ChatOrchestrator ────────────────────

    StandardResponse handleBlocking(
        const CreateChatCompletionRequest& request) override
    {
        validateGenerativeModel(request.model);
        return ChatOrchestrator::getInstance().handleBlocking(request);
    }

    void handleStreaming(
        const CreateChatCompletionRequest& request,
        StreamCallback callback) override
    {
        validateGenerativeModel(request.model);
        ChatOrchestrator::getInstance().handleStreaming(request, callback);
    }

    bool deleteSession(const std::string& completion_id) override {
        return ChatOrchestrator::getInstance().deleteSession(completion_id);
    }

    bool cancelSession(const std::string& completion_id) override {
        return ChatOrchestrator::getInstance().cancelSession(completion_id);
    }

    // ── Conventional AI path — routes to ConventionalAIOrchestrator ──────────

    TensorInferenceResponse handleInfer(
        const TensorInferenceRequest& request) override
    {
        // ConventionalAIOrchestrator validates model_type and selects backend
        return ConventionalAIOrchestrator::getInstance().handleInfer(request);
    }

private:
    InferenceRouter() = default;

    // Validates that the model exists and is a generative model.
    // Throws GenAIException if the model is not found or is conventional.
    void validateGenerativeModel(const std::string& model_id) {
        auto& cfg = ModelConfigManager::getInstance();

        if (!cfg.validateModel(model_id)) {
            throw GenAIException(
                GenAIErrorCode::MODEL_NOT_FOUND,
                "Model '" + model_id + "' not found. "
                "Check /v1/models for available models.",
                404);
        }

        const std::string model_type = cfg.getModelType(model_id);
        if (model_type == "conventional") {
            throw GenAIException(
                GenAIErrorCode::INVALID_REQUEST,
                "Model '" + model_id + "' is a conventional AI model. "
                "Use POST /v2/models/" + model_id + "/infer instead of "
                "/v1/chat/completions.",
                400);
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// IInferenceRouter::getInstance() — factory method
// ─────────────────────────────────────────────────────────────────────────────

IInferenceRouter& IInferenceRouter::getInstance() {
    return InferenceRouter::getInstance();
}
