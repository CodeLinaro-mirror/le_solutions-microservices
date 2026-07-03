// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/backend/IGenerativeBackend.h"
#include "qai_forge/backend/IInferenceBackend.h"
#include "qai_forge/orchestration/IOrchestrator.h"
#include <functional>
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
// Scheduler path creates backend+orchestrator pairs so each ModelRuntime can
// own its model handle, worker subprocess, and inference pipeline.
//
// Legacy path may still use singleton access until the scheduler-only cutover
// removes ChatOrchestratorImpl::handleBlocking/handleStreaming fallback.
//
// See docs/genai-backend-decoupling.md §8 for the full design rationale.
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// RuntimePair — Backend + Orchestrator pair owned by one ModelRuntime
//
// BackendFactory::createRuntimePair() returns one of these per model load.
// ModelRuntime takes ownership of both members.
// ─────────────────────────────────────────────────────────────────────────────
struct RuntimePair {
    std::unique_ptr<IGenerativeBackend> backend;
    std::unique_ptr<IOrchestrator>      orchestrator;
};

// Factory function type used by WarmModelPool to create RuntimePairs.
using ModelRuntimePairFactory =
    std::function<RuntimePair(const std::string& model_id)>;

// Factory function type used by PredictiveModelPool to create IInferenceBackend instances.
// Each call returns a new owned instance — not a singleton.
using PredictiveBackendFactory =
    std::function<std::unique_ptr<IInferenceBackend>(const std::string& model_id)>;

class BackendFactory {
public:
    // ── Generative AI backends ────────────────────────────────────────────────

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
     * Create a scheduler-owned (backend, orchestrator) pair for the given model.
     *
     * Called by WarmModelPool::getOrCreateRuntimeLocked() when a new
     * ModelRuntime is created. Each ModelRuntime owns its own pair so that
     * concurrent model loads do not share state.
     *
     * @param model_id  Model identifier from ModelConfig.
     */
    static RuntimePair createRuntimePair(const std::string& model_id);

    /**
     * Legacy singleton access.
     *
     * Kept until the old ChatOrchestratorImpl fallback path is removed.
     */
    static IGenerativeBackend& getGenerativeBackend(const std::string& runtime);

    // ── Predictive AI backends ────────────────────────────────────────────────

    /**
     * Create a scheduler-owned predictive backend instance for the given runtime.
     *
     * Returns a new owned IInferenceBackend — NOT a singleton. Each
     * PredictiveModelRuntime calls this to get its own backend instance,
     * enabling multiple models of the same runtime to coexist.
     *
     * @param runtime  Runtime identifier: "qnn", "qnn_context_binary",
     *                 "snpe", "qnn_dlc", "litert", "tflite"
     * @throws GenAIException(INVALID_REQUEST, 400) for unknown runtimes.
     */
    static std::unique_ptr<IInferenceBackend>
    createPredictiveBackend(const std::string& runtime);

    /**
     * Resolve a model id to its runtime and create a scheduler-owned
     * predictive backend instance.
     *
     * Called by PredictiveModelPool's default factory when no custom factory
     * is provided.
     *
     * @param model_id  Model identifier from ModelConfig.
     * @throws GenAIException(MODEL_NOT_FOUND, 404) if model is unknown.
     * @throws GenAIException(INVALID_REQUEST, 400) for unknown runtimes.
     */
    static std::unique_ptr<IInferenceBackend>
    createPredictiveBackendForModel(const std::string& model_id);

    // BackendFactory is a pure static utility — no instances.
    BackendFactory() = delete;
};
