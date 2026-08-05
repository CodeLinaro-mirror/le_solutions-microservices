// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// GenIEBackend — IGenerativeBackend implementation for Qualcomm GenIE SDK
//
// Wraps InferenceWorkerManager (LLM) and VlmInferenceWorkerManager (VLM).
// The static worker singletons previously in ChatOrchestratorImpl.cpp have
// been moved here — worker lifecycle is a backend concern, not an orchestrator
// concern.
//
// Key design decisions:
//   - bypass_think_filter is set here (GenIE SDK internal flag), not in Layer 2.
//   - Worker selection (LLM vs VLM) is based on ModelConfigManager::supportsVision().
//   - onContextCompacted() calls sendReset() on the correct worker.
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/backend/GenIEBackend.h"
#include "qai_forge/worker/InferenceWorkerManager.h"
#include "qai_forge/worker/VlmInferenceWorkerManager.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/Logger.h"

// ─────────────────────────────────────────────────────────────────────────────
// Worker singletons
//
// Previously these were static functions in ChatOrchestratorImpl.cpp.
// They belong here — worker lifecycle is a backend concern.
// ─────────────────────────────────────────────────────────────────────────────

static InferenceWorkerManager& getLlmWorker() {
    static InferenceWorkerManager llm_worker("llm");
    return llm_worker;
}

static VlmInferenceWorkerManager& getVlmWorker() {
    return VlmInferenceWorkerManager::getInstance();
}

// ─────────────────────────────────────────────────────────────────────────────
// Singleton
// ─────────────────────────────────────────────────────────────────────────────

GenIEBackend& GenIEBackend::getInstance() {
    static GenIEBackend instance;
    return instance;
}

// ─────────────────────────────────────────────────────────────────────────────
// capabilities()
// ─────────────────────────────────────────────────────────────────────────────

BackendCapabilities GenIEBackend::capabilities() const {
    auto& cfg = ModelConfigManager::getInstance();
    return BackendCapabilities{
        // GenIE has a stateful KV cache — reset it after context compaction
        .context_strategy             = ContextStrategy::RESET_KV,
        // Use the current model's context window; fall back to 4096 if none loaded
        .context_window               = current_model_id_.empty()
                                            ? 4096
                                            : cfg.getContextSize(current_model_id_),
        .compaction_threshold         = 0.70f,
        // Snapdragon DSP: only one inference at a time
        .concurrency_model            = ConcurrencyModel::EXCLUSIVE,
        .max_concurrent               = 1,
        // GenIE SDK can filter <think> tokens internally via bypass_think_filter
        .backend_filters_think_tokens = true,
        // LLM supports KV save/restore (GenieDialog_save/restore); VLM does not
        .supports_kv_save_restore     = !current_is_vlm_,
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// ensureWorkerRunning()
// ─────────────────────────────────────────────────────────────────────────────

void GenIEBackend::ensureWorkerRunning(const std::string& model_id,
                                       const std::string& config_file,
                                       const std::string& sampler_file) {
    current_model_id_ = model_id;
    current_is_vlm_   = ModelConfigManager::getInstance().supportsVision(model_id);

    if (current_is_vlm_) {
        LOG_DEBUG("[GenIEBackend] ensureWorkerRunning: VLM model=" << model_id);
        getVlmWorker().ensureWorkerRunning(model_id, config_file, sampler_file);
    } else {
        LOG_DEBUG("[GenIEBackend] ensureWorkerRunning: LLM model=" << model_id);
        getLlmWorker().ensureWorkerRunning(model_id, config_file, sampler_file);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// generate() — LLM text inference
// ─────────────────────────────────────────────────────────────────────────────

void GenIEBackend::generate(
    const std::string& event_id,
    const std::string& prompt,
    bool               streaming,
    int                max_tokens,
    float              temperature,
    float              top_p,
    int                top_k,
    float              presence_penalty,
    float              frequency_penalty,
    bool               use_reasoning,
    std::function<void(const IPCTokenEvent&)>  on_token,
    std::function<void(const IPCDoneEvent&)>   on_done,
    std::function<void(const IPCErrorEvent&)>  on_error)
{
    // bypass_think_filter is a GenIE SDK internal flag.
    // The orchestrator passes use_reasoning (decided by model capability + budget);
    // GenIEBackend maps it to bypass_think_filter here — invisible to Layer 2.
    const bool bypass_think_filter = use_reasoning;

    getLlmWorker().executeRequest(
        event_id,
        prompt,
        streaming,
        max_tokens,
        temperature,
        top_p,
        top_k,
        presence_penalty,
        frequency_penalty,
        bypass_think_filter,
        on_token,
        on_done,
        on_error);
}

// ─────────────────────────────────────────────────────────────────────────────
// generateVlm() — VLM vision+text inference
// ─────────────────────────────────────────────────────────────────────────────

void GenIEBackend::generateVlm(
    const std::string&              event_id,
    const std::string&              prompt,
    const std::vector<std::string>& image_paths,
    bool                            streaming,
    int                             max_tokens,
    float                           temperature,
    float                           top_p,
    int                             top_k,
    float                           presence_penalty,
    float                           frequency_penalty,
    std::function<void(const IPCTokenEvent&)>  on_token,
    std::function<void(const IPCDoneEvent&)>   on_done,
    std::function<void(const IPCErrorEvent&)>  on_error)
{
    getVlmWorker().executeVlmRequest(
        event_id,
        prompt,
        image_paths,
        streaming,
        max_tokens,
        temperature,
        top_p,
        top_k,
        presence_penalty,
        frequency_penalty,
        on_token,
        on_done,
        on_error);
}

// ─────────────────────────────────────────────────────────────────────────────
// onContextCompacted()
// ─────────────────────────────────────────────────────────────────────────────

void GenIEBackend::onContextCompacted() {
    // GenIE-specific: reset the KV cache after summarization so the next turn
    // starts fresh with the compacted context.
    // Select the correct worker based on the current model type.
    LOG_INFO("[GenIEBackend] Context compacted — resetting KV cache for model: "
             << current_model_id_
             << (current_is_vlm_ ? " (VLM)" : " (LLM)"));
    try {
        if (current_is_vlm_) {
            getVlmWorker().sendReset();
        } else {
            getLlmWorker().sendReset();
        }
    } catch (const std::exception& e) {
        LOG_WARN("[GenIEBackend] KV cache reset failed: " << e.what()
                 << " (non-fatal — continuing with summary)");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// KV cache operations (LLM only)
// ─────────────────────────────────────────────────────────────────────────────

void GenIEBackend::saveKv(const std::string& name) {
    getLlmWorker().saveKvCache(name);
}

void GenIEBackend::restoreKv(const std::string& name) {
    getLlmWorker().restoreKvCache(name);
}

void GenIEBackend::resetKv() {
    getLlmWorker().sendReset();
}

// ─────────────────────────────────────────────────────────────────────────────
// terminateWorker()
// ─────────────────────────────────────────────────────────────────────────────

void GenIEBackend::terminateWorker(bool force) {
    if (current_is_vlm_) {
        getVlmWorker().terminateWorker(force);
    } else {
        getLlmWorker().terminateWorker(force);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// isHealthy()
// ─────────────────────────────────────────────────────────────────────────────

bool GenIEBackend::isHealthy() const {
    if (current_is_vlm_) {
        return getVlmWorker().isWorkerRunning();
    }
    return getLlmWorker().isWorkerRunning();
}
