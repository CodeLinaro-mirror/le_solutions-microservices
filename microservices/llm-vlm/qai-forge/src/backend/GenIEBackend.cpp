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
#include "qai_forge/InternalDTOs.h"
#include "qai_forge/worker/InferenceWorkerManager.h"
#include "qai_forge/worker/VlmInferenceWorkerManager.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/Logger.h"
#include <memory>

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

GenIEBackend::GenIEBackend() = default;

GenIEBackend::~GenIEBackend() = default;

// ─────────────────────────────────────────────────────────────────────────────
// Singleton
// ─────────────────────────────────────────────────────────────────────────────

GenIEBackend& GenIEBackend::getInstance() {
    static GenIEBackend instance;
    return instance;
}

InferenceWorkerManager& GenIEBackend::llmWorker() {
    if (!llm_worker_) {
        llm_worker_ = std::make_unique<InferenceWorkerManager>("llm");
    }
    return *llm_worker_;
}

VlmInferenceWorkerManager& GenIEBackend::vlmWorker() {
    if (!vlm_worker_) {
        vlm_worker_ = std::make_unique<VlmInferenceWorkerManager>();
    }
    return *vlm_worker_;
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
        // Per backend instance / loaded GenIE handle: one request at a time.
        // Cross-model concurrency comes from separate scheduler-owned backends.
        .concurrency_model            = ConcurrencyModel::EXCLUSIVE,
        .max_concurrent               = 1,
        // GenIE SDK can filter <think> tokens internally via bypass_think_filter
        .backend_filters_think_tokens = true,
        // LLM supports KV save/restore (GenieDialog_save/restore); VLM does not
        .supports_kv_save_restore     = !current_is_vlm_,
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// loadModel() / unloadModel()
// ─────────────────────────────────────────────────────────────────────────────

void GenIEBackend::loadModel(const std::string& model_id) {
    const auto* model_config =
        ModelConfigManager::getInstance().getModelConfig(model_id);
    if (!model_config) {
        throw GenAIException(
            GenAIErrorCode::MODEL_NOT_FOUND,
            "Model '" + model_id + "' not found. Check /v1/models for available models.",
            404);
    }

    ensureWorkerRunning(model_id,
                        model_config->config_file,
                        model_config->sampler_config_file);
}

void GenIEBackend::unloadModel(bool force) {
    LOG_INFO("[GenIEBackend] unloadModel: model=" << current_model_id_
             << ", force=" << (force ? "true" : "false"));

    if (vlm_worker_) {
        if (force) {
            vlm_worker_->terminateWorker(true);
        } else {
            vlm_worker_->shutdown();
        }
        vlm_worker_.reset();
    }

    if (llm_worker_) {
        if (force) {
            llm_worker_->terminateWorker(true);
        } else {
            llm_worker_->shutdown();
        }
        llm_worker_.reset();
    }

    current_model_id_.clear();
    current_is_vlm_ = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// ensureWorkerRunning()
// ─────────────────────────────────────────────────────────────────────────────

void GenIEBackend::ensureWorkerRunning(const std::string& model_id,
                                       const std::string& config_file,
                                       const std::string& sampler_file) {
    const bool is_vlm = ModelConfigManager::getInstance().supportsVision(model_id);
    if (!current_model_id_.empty() && current_is_vlm_ != is_vlm) {
        unloadModel(false);
    }

    current_model_id_ = model_id;
    current_is_vlm_   = is_vlm;

    if (current_is_vlm_) {
        LOG_DEBUG("[GenIEBackend] ensureWorkerRunning: VLM model=" << model_id);
        vlmWorker().ensureWorkerRunning(model_id, config_file, sampler_file);
    } else {
        LOG_DEBUG("[GenIEBackend] ensureWorkerRunning: LLM model=" << model_id);
        llmWorker().ensureWorkerRunning(model_id, config_file, sampler_file);
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

    llmWorker().executeRequest(
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
    vlmWorker().executeVlmRequest(
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
//
// No-op: KV cache reset is now handled by resetKvAsync() which is called by
// GenieOrchestrator immediately after every generate() / generateVlm() call.
// The eager background reset runs concurrently with returning the response to
// the HTTP layer, eliminating the pre-inference reset latency.
// Kept for interface compatibility with IGenerativeBackend.
// ─────────────────────────────────────────────────────────────────────────────

void GenIEBackend::onContextCompacted() {
    LOG_DEBUG("[GenIEBackend] onContextCompacted() called — no-op "
              "(KV reset handled by resetKvAsync after each inference)");
}

// ─────────────────────────────────────────────────────────────────────────────
// resetKvAsync() — Eager background KV cache reset
//
// Called by GenieOrchestrator immediately after generate() / generateVlm()
// returns. Initiates a background reset on the active worker so the reset
// runs concurrently with returning the response to the HTTP layer.
// The next generate() call waits for the reset to complete via
// InferenceWorkerManager::waitForPendingReset() (called inside executeRequest).
// ─────────────────────────────────────────────────────────────────────────────

void GenIEBackend::resetKvAsync() {
    LOG_INFO("[GenIEBackend] Initiating background KV reset for model: "
             << current_model_id_
             << (current_is_vlm_ ? " (VLM)" : " (LLM)"));
    try {
        if (current_is_vlm_) {
            if (vlm_worker_) {
                vlm_worker_->initiateBackgroundReset();
            }
        } else {
            if (llm_worker_) {
                llm_worker_->initiateBackgroundReset();
            }
        }
    } catch (const std::exception& e) {
        LOG_WARN("[GenIEBackend] Failed to initiate background KV reset: " << e.what()
                 << " (non-fatal — next request will rebuild from scratch)");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// KV cache operations (LLM only)
// ─────────────────────────────────────────────────────────────────────────────

void GenIEBackend::saveKv(const std::string& name) {
    if (!llm_worker_) {
        LOG_WARN("[GenIEBackend] saveKv skipped; no LLM worker loaded");
        return;
    }
    llm_worker_->saveKvCache(name);
}

void GenIEBackend::restoreKv(const std::string& name) {
    if (!llm_worker_) {
        LOG_WARN("[GenIEBackend] restoreKv skipped; no LLM worker loaded");
        return;
    }
    llm_worker_->restoreKvCache(name);
}

void GenIEBackend::resetKv() {
    if (!llm_worker_) {
        LOG_WARN("[GenIEBackend] resetKv skipped; no LLM worker loaded");
        return;
    }
    llm_worker_->sendReset();
}

// ─────────────────────────────────────────────────────────────────────────────
// terminateWorker()
// ─────────────────────────────────────────────────────────────────────────────

void GenIEBackend::terminateWorker(bool force) {
    if (current_is_vlm_) {
        if (vlm_worker_) {
            vlm_worker_->terminateWorker(force);
        }
    } else {
        if (llm_worker_) {
            llm_worker_->terminateWorker(force);
        }
    }
    current_model_id_.clear();
    current_is_vlm_ = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// isHealthy()
// ─────────────────────────────────────────────────────────────────────────────

bool GenIEBackend::isHealthy() const {
    if (current_is_vlm_) {
        return vlm_worker_ && vlm_worker_->isWorkerRunning();
    }
    return llm_worker_ && llm_worker_->isWorkerRunning();
}
