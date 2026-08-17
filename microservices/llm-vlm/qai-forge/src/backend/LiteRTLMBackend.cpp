// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// LiteRTLMBackend — Phase 3: IGenerativeBackend implementation for LiteRT-LM
//
// Wraps LiteRTLMWorkerManager (which extends InferenceWorkerManager) to manage
// the litert-lm-inference-worker subprocess. The worker uses the LiteRT-LM
// Session API for raw inference; all chat intelligence lives in
// LiteRTLMOrchestrator (Phase 4).
//
// Key differences from the old singleton-based implementation:
//   - Uses LiteRTLMWorkerManager instead of plain InferenceWorkerManager
//   - Exposes getMetadata() for LiteRTLMOrchestrator initialization
//   - unloadModel() always uses SIGKILL for clean DSP resource reclamation
//   - Supports scheduler-owned instances (one per ModelRuntime)
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/backend/LiteRTLMBackend.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/Logger.h"

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

LiteRTLMBackend::LiteRTLMBackend() = default;

// ─────────────────────────────────────────────────────────────────────────────
// Singleton (legacy path — kept for BackendFactory::getGenerativeBackend)
// ─────────────────────────────────────────────────────────────────────────────

LiteRTLMBackend& LiteRTLMBackend::getInstance() {
    static LiteRTLMBackend instance;
    return instance;
}

// ─────────────────────────────────────────────────────────────────────────────
// worker() — lazy-initialize the LiteRTLMWorkerManager
// ─────────────────────────────────────────────────────────────────────────────

LiteRTLMWorkerManager& LiteRTLMBackend::worker() {
    if (!worker_) {
        worker_ = std::make_unique<LiteRTLMWorkerManager>();
    }
    return *worker_;
}

// ─────────────────────────────────────────────────────────────────────────────
// capabilities()
// ─────────────────────────────────────────────────────────────────────────────

BackendCapabilities LiteRTLMBackend::capabilities() const {
    auto& cfg = ModelConfigManager::getInstance();
    return BackendCapabilities{
        // LiteRT-LM uses a stateful KV cache — reset after context compaction
        .context_strategy             = ContextStrategy::RESET_KV,
        .context_window               = current_model_id_.empty()
                                            ? 4096
                                            : cfg.getContextSize(current_model_id_),
        .compaction_threshold         = 0.70f,
        // Qualcomm NPU: only one inference at a time per backend instance
        .concurrency_model            = ConcurrencyModel::EXCLUSIVE,
        .max_concurrent               = 1,
        // LiteRT-LM does not filter <think> tokens internally
        .backend_filters_think_tokens = false,
        // KV save/restore not yet supported for LiteRT-LM
        .supports_kv_save_restore     = false,
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// loadModel()
// ─────────────────────────────────────────────────────────────────────────────

void LiteRTLMBackend::loadModel(const std::string& model_id) {
    const auto* model_config =
        ModelConfigManager::getInstance().getModelConfig(model_id);
    if (!model_config) {
        throw GenAIException(
            GenAIErrorCode::MODEL_NOT_FOUND,
            "Model '" + model_id + "' not found. Check /v1/models for available models.",
            404);
    }

    LOG_INFO("[LiteRTLMBackend] loadModel: model=" << model_id
             << " config=" << model_config->config_file);

    ensureWorkerRunning(model_id, model_config->config_file, "");
}

// ─────────────────────────────────────────────────────────────────────────────
// unloadModel()
//
// Always uses SIGKILL (force=true) to ensure the kernel's QNN driver exit
// handler reclaims all DSP contexts cleanly. Same rationale as GenIEBackend.
// ─────────────────────────────────────────────────────────────────────────────

void LiteRTLMBackend::unloadModel(bool /*force*/) {
    LOG_INFO("[LiteRTLMBackend] unloadModel: model=" << current_model_id_
             << " (always SIGKILL for clean DSP reclamation)");

    if (worker_) {
        worker_->terminateWorker(true); // Always SIGKILL
        worker_.reset();
    }

    current_model_id_.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// ensureWorkerRunning()
// ─────────────────────────────────────────────────────────────────────────────

void LiteRTLMBackend::ensureWorkerRunning(const std::string& model_id,
                                           const std::string& config_file,
                                           const std::string& sampler_config) {
    current_model_id_ = model_id;
    LOG_DEBUG("[LiteRTLMBackend] ensureWorkerRunning: model=" << model_id
              << " config=" << config_file);
    worker().ensureWorkerRunning(model_id, config_file, sampler_config);
}

// ─────────────────────────────────────────────────────────────────────────────
// generate() — LLM text inference
// ─────────────────────────────────────────────────────────────────────────────

void LiteRTLMBackend::generate(
    const std::string& event_id,
    const std::string& prompt,
    bool               streaming,
    int                max_tokens,
    float              temperature,
    float              top_p,
    int                top_k,
    float              presence_penalty,
    float              frequency_penalty,
    bool               /*use_reasoning*/,
    std::function<void(const IPCTokenEvent&)>  on_token,
    std::function<void(const IPCDoneEvent&)>   on_done,
    std::function<void(const IPCErrorEvent&)>  on_error)
{
    worker().executeRequest(
        event_id,
        prompt,
        streaming,
        max_tokens,
        temperature,
        top_p,
        top_k,
        presence_penalty,
        frequency_penalty,
        false, // bypass_think_filter — not applicable for LiteRT-LM
        on_token,
        on_done,
        on_error);
}

void LiteRTLMBackend::generateStructured(
    const std::string& event_id,
    const json& messages,
    const json& tools,
    bool streaming,
    int max_tokens,
    float temperature,
    float top_p,
    int top_k,
    float presence_penalty,
    float frequency_penalty,
    std::function<void(const IPCTokenEvent&)> on_token,
    std::function<void(const IPCDoneEvent&)> on_done,
    std::function<void(const IPCErrorEvent&)> on_error) {
    worker().executeStructuredRequest(
        event_id,
        messages,
        tools,
        streaming,
        max_tokens,
        temperature,
        top_p,
        top_k,
        presence_penalty,
        frequency_penalty,
        std::move(on_token),
        std::move(on_done),
        std::move(on_error));
}

// ─────────────────────────────────────────────────────────────────────────────
// generateVlm() — not supported by LiteRT-LM (LLM-only backend)
// ─────────────────────────────────────────────────────────────────────────────

void LiteRTLMBackend::generateVlm(
    const std::string&              event_id,
    const std::string&              /*prompt*/,
    const std::vector<std::string>& /*image_paths*/,
    bool                            /*streaming*/,
    int                             /*max_tokens*/,
    float                           /*temperature*/,
    float                           /*top_p*/,
    int                             /*top_k*/,
    float                           /*presence_penalty*/,
    float                           /*frequency_penalty*/,
    std::function<void(const IPCTokenEvent&)>  /*on_token*/,
    std::function<void(const IPCDoneEvent&)>   /*on_done*/,
    std::function<void(const IPCErrorEvent&)>  on_error)
{
    on_error({event_id, "", "LiteRTLMBackend does not support VLM inference"});
}

// ─────────────────────────────────────────────────────────────────────────────
// onContextCompacted()
// ─────────────────────────────────────────────────────────────────────────────

void LiteRTLMBackend::onContextCompacted() {
    LOG_INFO("[LiteRTLMBackend] Context compacted — resetting KV cache for model: "
             << current_model_id_);
    try {
        worker().sendReset();
    } catch (const std::exception& e) {
        LOG_WARN("[LiteRTLMBackend] KV cache reset failed: " << e.what()
                 << " (non-fatal — continuing with summary)");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// resetKvAsync() — Eager background KV cache reset
// ─────────────────────────────────────────────────────────────────────────────

void LiteRTLMBackend::resetKvAsync() {
    LOG_INFO("[LiteRTLMBackend] Initiating background KV reset for model: "
             << current_model_id_);
    try {
        if (worker_) {
            worker_->initiateBackgroundReset();
        }
    } catch (const std::exception& e) {
        LOG_WARN("[LiteRTLMBackend] Failed to initiate background KV reset: " << e.what()
                 << " (non-fatal)");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// KV cache operations
// ─────────────────────────────────────────────────────────────────────────────

void LiteRTLMBackend::saveKv(const std::string& name) {
    // LiteRT-LM KV save not yet supported — log and continue
    LOG_WARN("[LiteRTLMBackend] saveKv('" << name << "') not supported for LiteRT-LM");
}

void LiteRTLMBackend::restoreKv(const std::string& name) {
    // LiteRT-LM KV restore not yet supported — log and continue
    LOG_WARN("[LiteRTLMBackend] restoreKv('" << name << "') not supported for LiteRT-LM");
}

void LiteRTLMBackend::resetKv() {
    try {
        worker().sendReset();
    } catch (const std::exception& e) {
        LOG_WARN("[LiteRTLMBackend] resetKv failed: " << e.what());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// terminateWorker()
// ─────────────────────────────────────────────────────────────────────────────

void LiteRTLMBackend::terminateWorker(bool force) {
    if (worker_) {
        worker_->terminateWorker(force);
    }
    current_model_id_.clear();
}

bool LiteRTLMBackend::forceKillActiveWorker() {
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// isHealthy()
// ─────────────────────────────────────────────────────────────────────────────

bool LiteRTLMBackend::isHealthy() const {
    return worker_ && worker_->isWorkerRunning();
}

// ─────────────────────────────────────────────────────────────────────────────
// getMetadata() — Return metadata received from worker after model load
// ─────────────────────────────────────────────────────────────────────────────

const LiteRTLMMetadata& LiteRTLMBackend::getMetadata() const {
    static const LiteRTLMMetadata empty_metadata;
    if (!worker_) return empty_metadata;
    return worker_->getMetadata();
}
