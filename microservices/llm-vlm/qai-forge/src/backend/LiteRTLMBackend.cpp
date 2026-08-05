// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// LiteRTLMBackend — IGenerativeBackend implementation for LiteRT-LM
//
// Delegates to InferenceWorkerManager with process_type "litert-lm".
// The worker binary (litert-lm-inference-worker) uses LiteRT-LM's
// LlmInference C++ API to run LLM inference on the Qualcomm NPU.
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/backend/LiteRTLMBackend.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/Logger.h"

// ─────────────────────────────────────────────────────────────────────────────
// Worker singleton
// ─────────────────────────────────────────────────────────────────────────────

static InferenceWorkerManager& getLiteRTLMWorker() {
    static InferenceWorkerManager worker("litert-lm");
    return worker;
}

// ─────────────────────────────────────────────────────────────────────────────
// Singleton
// ─────────────────────────────────────────────────────────────────────────────

LiteRTLMBackend::LiteRTLMBackend() = default;

LiteRTLMBackend& LiteRTLMBackend::getInstance() {
    static LiteRTLMBackend instance;
    return instance;
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
        // Qualcomm NPU: only one inference at a time
        .concurrency_model            = ConcurrencyModel::EXCLUSIVE,
        .max_concurrent               = 1,
        // LiteRT-LM does not filter <think> tokens internally
        .backend_filters_think_tokens = false,
        // KV save/restore support depends on LiteRT-LM version
        .supports_kv_save_restore     = false,
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// ensureWorkerRunning()
// ─────────────────────────────────────────────────────────────────────────────

void LiteRTLMBackend::ensureWorkerRunning(const std::string& model_id,
                                           const std::string& config_file,
                                           const std::string& sampler_config) {
    current_model_id_ = model_id;
    LOG_DEBUG("[LiteRTLMBackend] ensureWorkerRunning: model=" << model_id);
    getLiteRTLMWorker().ensureWorkerRunning(model_id, config_file, sampler_config);
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
    getLiteRTLMWorker().executeRequest(
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
        getLiteRTLMWorker().sendReset();
    } catch (const std::exception& e) {
        LOG_WARN("[LiteRTLMBackend] KV cache reset failed: " << e.what()
                 << " (non-fatal — continuing with summary)");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// KV cache operations
// ─────────────────────────────────────────────────────────────────────────────

void LiteRTLMBackend::saveKv(const std::string& name) {
    getLiteRTLMWorker().saveKvCache(name);
}

void LiteRTLMBackend::restoreKv(const std::string& name) {
    getLiteRTLMWorker().restoreKvCache(name);
}

void LiteRTLMBackend::resetKv() {
    getLiteRTLMWorker().sendReset();
}

// ─────────────────────────────────────────────────────────────────────────────
// terminateWorker() / isHealthy()
// ─────────────────────────────────────────────────────────────────────────────

void LiteRTLMBackend::terminateWorker(bool force) {
    getLiteRTLMWorker().terminateWorker(force);
}

bool LiteRTLMBackend::isHealthy() const {
    return getLiteRTLMWorker().isWorkerRunning();
}
