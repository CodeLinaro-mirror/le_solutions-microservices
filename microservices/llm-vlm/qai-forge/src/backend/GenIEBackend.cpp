// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// GenIEBackend — qai_forge::IGenerativeBackend implementation for Qualcomm GenIE SDK
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
//   - loadModel() derives the placed HTP device_id from the resolved
//     ExecutionPlan and materializes a runtime-specific GenIE config tree via
//     GenieConfigMaterializer. Placement is the only path into loadModel() —
//     model-only callers get a default RuntimeId(model_id, 0).
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/backend/GenIEBackend.h"
#include "qai_forge/InternalDTOs.h"
#include "qai_forge/worker/InferenceWorkerManager.h"
#include "qai_forge/worker/VlmInferenceWorkerManager.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/Logger.h"
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

// Extracts the placed HTP device id from a GenIE ExecutionPlan. Throws for
// malformed, non-GenIE, or automatic (no physical target) plans.
int selectedGenieDeviceId(const qai_forge::execution::ExecutionPlan& plan) {
    if (plan.provider != "genie") {
        throw std::invalid_argument(
            "GenIEBackend: expected a GenIE execution plan");
    }
    if (plan.components.empty()) {
        throw std::invalid_argument(
            "GenIEBackend: placement plan has no GenIE components");
    }

    std::optional<int> selected;
    for (const auto& component : plan.components) {
        if (component.target.provider != "genie") {
            throw std::invalid_argument(
                "GenIEBackend: plan contains a non-GenIE target");
        }
        if (component.target.semantics !=
                qai_forge::execution::TargetSemantics::PhysicalInstance ||
            component.target.target_id.rfind("device_id=", 0) != 0) {
            throw std::invalid_argument(
                "GenIEBackend: placement plan must resolve to a physical "
                "device_id target (no physical HTP target was found in the "
                "resolved topology — set QAI_FORGE_PLATFORM=ventunoq|iq9 to "
                "use a built-in hardware preset, or QNN_BACKEND_LIB=<path to "
                "libQnnHtp.so> to auto-probe the topology at startup)");
        }

        const std::string value =
            component.target.target_id.substr(std::string("device_id=").size());
        size_t consumed = 0;
        int parsed = 0;
        try {
            parsed = std::stoi(value, &consumed, 10);
        } catch (...) {
            throw std::invalid_argument(
                "GenIEBackend: invalid GenIE device_id: " + value);
        }
        if (consumed != value.size() || parsed < 0) {
            throw std::invalid_argument(
                "GenIEBackend: invalid GenIE device_id: " + value);
        }

        if (selected.has_value() && *selected != parsed) {
            throw std::invalid_argument(
                "GenIEBackend: plan contains conflicting device IDs");
        }
        selected = parsed;
    }

    if (!selected.has_value()) {
        throw std::invalid_argument(
            "GenIEBackend: placement plan has no physical GenIE device");
    }
    return *selected;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

GenIEBackend::GenIEBackend() = default;

GenIEBackend::~GenIEBackend() = default;

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

int GenIEBackend::effectiveMaxConcurrent(const std::string& model_id) const {
    auto& cfg = ModelConfigManager::getInstance();
    const int max_slots = cfg.getMaxSlots(model_id);
    const bool is_vlm = cfg.supportsVision(model_id);
    const bool shared_engine = cfg.hasSharedEngine(model_id);
    return (!is_vlm && max_slots > 1 && shared_engine) ? max_slots : 1;
}

BackendCapabilities GenIEBackend::capabilities() const {
    auto& cfg = ModelConfigManager::getInstance();
    const int max_concurrent = current_model_id_.empty()
        ? 1
        : effectiveMaxConcurrent(current_model_id_);
    const bool bounded = max_concurrent > 1;
    return BackendCapabilities{
        // GenIE has a stateful KV cache — reset it after context compaction
        .context_strategy             = ContextStrategy::RESET_KV,
        // Use the current model's context window; fall back to 4096 if none loaded
        .context_window               = current_model_id_.empty()
                                            ? 4096
                                            : cfg.getContextSize(current_model_id_),
        .compaction_threshold         = 0.70f,
        // BOUNDED when the loaded model's genie_config.json declares
        // dialog.engine.batching.max-slots > 1 (continuous batching enabled);
        // otherwise EXCLUSIVE/1, matching every model's behavior before
        // continuous batching existed.
        .concurrency_model            = bounded ? ConcurrencyModel::BOUNDED
                                                 : ConcurrencyModel::EXCLUSIVE,
        .max_concurrent               = max_concurrent,
        // GenIE SDK can filter <think> tokens internally via bypass_think_filter
        .backend_filters_think_tokens = true,
        // LLM supports KV save/restore (GenieDialog_save/restore); VLM does not
        .supports_kv_save_restore     = !current_is_vlm_.load(),
        // Mirrors current_is_vlm_ — tells orchestrators to call generateVlm()
        .supports_vision              = current_is_vlm_.load(),
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// loadModel() / unloadModel()
// ─────────────────────────────────────────────────────────────────────────────

void GenIEBackend::loadModel(const std::string& model_id) {
    throw std::runtime_error(
        "GenIEBackend: loadModel(model_id) is not supported — placement is "
        "mandatory. Use loadModel(RuntimeLaunchRequest) or "
        "loadModel(model_id, ExecutionPlan).");
}

void GenIEBackend::loadModel(
    const qai_forge::execution::RuntimeLaunchRequest& request) {
    loadModel(request.model_id, request.runtime_id, request.execution_plan);
}

void GenIEBackend::loadModel(
    const std::string& model_id,
    const qai_forge::execution::ExecutionPlan& plan) {
    loadModel(model_id, qai_forge::execution::RuntimeId{model_id, 0}, plan);
}

void GenIEBackend::loadModel(
    const std::string& model_id,
    const qai_forge::execution::RuntimeId& runtime_id,
    const qai_forge::execution::ExecutionPlan& plan) {
    const auto model_config =
        ModelConfigManager::getInstance().getModelConfig(model_id);
    if (!model_config) {
        throw GenAIException(
            GenAIErrorCode::MODEL_NOT_FOUND,
            "Model '" + model_id + "' not found. Check /v1/models for available models.",
            404);
    }

    const int device_id = selectedGenieDeviceId(plan);
    qai_forge::execution::GenieConfigMaterializer materializer;
    const auto launch = materializer.materialize(
        model_config->config_file,
        model_config->sampler_config_file,
        runtime_id,
        device_id,
        model_config.generationToken());

    ensureWorkerRunning(
        model_id,
        launch.config_file.string(),
        launch.sampler_config_file.string(),
        /*validate_ready=*/true);
    runtime_artifacts_ = launch.artifacts;
}

void GenIEBackend::unloadModel(bool force) {
    LOG_INFO("[GenIEBackend] unloadModel: model=" << current_model_id_
             << ", force=" << (force ? "true" : "false"));

    // Always use SIGKILL (terminateWorker(true)) to ensure the kernel's QNN driver
    // exit handler reclaims all contexts cleanly. The worker's graceful shutdown
    // (SIGTERM) can trigger buggy cleanup code that calls QnnDevice_free before
    // freeing all QNN contexts, leaving the DSP in a bad state that prevents
    // subsequent workers from initializing ("Failed to create dialog" error).
    // SIGKILL prevents the buggy cleanup from running and lets the kernel reclaim
    // QNN resources cleanly via the driver's process-exit handler.

    if (vlm_worker_) {
        vlm_worker_->terminateWorker(true);
        vlm_worker_.reset();
    }

    if (llm_worker_) {
        llm_worker_->terminateWorker(true);
        llm_worker_.reset();
    }

    current_model_id_.clear();
    current_is_vlm_.store(false);
    runtime_artifacts_.reset();
}

// ─────────────────────────────────────────────────────────────────────────────
// ensureWorkerRunning()
// ─────────────────────────────────────────────────────────────────────────────

void GenIEBackend::ensureWorkerRunning(const std::string& model_id,
                                       const std::string& config_file,
                                       const std::string& sampler_file) {
    ensureWorkerRunning(model_id, config_file, sampler_file, /*validate_ready=*/false);
}

void GenIEBackend::ensureWorkerRunning(const std::string& model_id,
                                       const std::string& config_file,
                                       const std::string& sampler_file,
                                       bool validate_ready) {
    const bool is_vlm = ModelConfigManager::getInstance().supportsVision(model_id);
    const int max_concurrent = effectiveMaxConcurrent(model_id);
    if (!current_model_id_.empty() && current_is_vlm_.load() != is_vlm) {
        unloadModel(false);
    }

    current_model_id_ = model_id;
    current_is_vlm_.store(is_vlm);

    ReadyValidator validator;
    if (validate_ready) {
        validator = [](const json& ready) {
            if (ready.value("provider", "") != "genie" ||
                ready.value("binding_strength", "") != "preferred") {
                throw std::runtime_error(
                    "GenIEBackend: placement-aware READY metadata was not "
                    "confirmed");
            }
        };
    }

    if (is_vlm) {
        LOG_DEBUG("[GenIEBackend] ensureWorkerRunning: VLM model=" << model_id);
        vlmWorker().setMaxSlots(max_concurrent);
        vlmWorker().ensureWorkerRunning(model_id, config_file, sampler_file, validator);
    } else {
        LOG_DEBUG("[GenIEBackend] ensureWorkerRunning: LLM model=" << model_id);
        llmWorker().setMaxSlots(max_concurrent);
        llmWorker().ensureWorkerRunning(model_id, config_file, sampler_file, validator);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// generate() — LLM text inference
// ─────────────────────────────────────────────────────────────────────────────

void GenIEBackend::generate(
    const std::string& event_id,
    const std::string& session_id,
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
        session_id,
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
    const std::string&              session_id,
    const std::string&              prompt,
    const std::vector<std::vector<uint8_t>>& images,
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
        session_id,
        prompt,
        images,
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
// No-op: KV reset is initiated by resetKvAsync() for both LLM and VLM.
// Kept for interface compatibility with qai_forge::IGenerativeBackend.
// ─────────────────────────────────────────────────────────────────────────────

void GenIEBackend::onContextCompacted() {
    LOG_DEBUG("[GenIEBackend] onContextCompacted() called — no-op "
              "(KV reset handled by resetKvAsync after each inference)");
}

// ─────────────────────────────────────────────────────────────────────────────
// resetKvAsync() — Eager background KV cache reset
//
// Called by GenieOrchestrator immediately after generate() / generateVlm()
// returns. The parent starts the reset asynchronously; the worker lane remains
// reserved until the matching RESET command completes.
// ─────────────────────────────────────────────────────────────────────────────

void GenIEBackend::resetKvAsync(const std::string& session_id) {
    const bool is_vlm = current_is_vlm_.load();
    // Demoted to DEBUG — this fires unconditionally after *every* inference
    // call (GenieOrchestrator's eager background-reset pattern), so at INFO
    // it was pure per-request bookkeeping noise with no diagnostic value
    // beyond confirming the call happened. Failures are still logged at WARN
    // below.
    LOG_DEBUG("[GenIEBackend] Initiating background KV reset for model: "
             << current_model_id_
             << (is_vlm ? " (VLM)" : " (LLM)")
             << " session=" << session_id);
    try {
        if (is_vlm) {
            if (vlm_worker_) {
                vlm_worker_->initiateBackgroundReset(session_id);
            }
        } else {
            if (llm_worker_) {
                llm_worker_->initiateBackgroundReset(session_id);
            }
        }
    } catch (const std::exception& e) {
        LOG_WARN("[GenIEBackend] Failed to initiate background KV reset: " << e.what()
                 << " (non-fatal — next request will rebuild from scratch)");
    }
}

void GenIEBackend::waitForSessionReady(const std::string& session_id) {
    if (current_is_vlm_.load()) {
        if (vlm_worker_) vlm_worker_->waitForPendingReset(session_id);
    } else if (llm_worker_) {
        llm_worker_->waitForSessionReady(session_id);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// KV cache operations (LLM only)
// ─────────────────────────────────────────────────────────────────────────────

void GenIEBackend::saveKv(const std::string& name, const std::string& session_id) {
    if (!llm_worker_) {
        LOG_WARN("[GenIEBackend] saveKv skipped; no LLM worker loaded");
        return;
    }
    llm_worker_->saveKvCache(name, session_id);
}

void GenIEBackend::restoreKv(const std::string& name, const std::string& session_id) {
    if (!llm_worker_) {
        LOG_WARN("[GenIEBackend] restoreKv skipped; no LLM worker loaded");
        return;
    }
    llm_worker_->restoreKvCache(name, session_id);
}

void GenIEBackend::resetKv(const std::string& session_id) {
    if (!llm_worker_) {
        LOG_WARN("[GenIEBackend] resetKv skipped; no LLM worker loaded");
        return;
    }
    llm_worker_->sendReset(session_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// terminateWorker()
// ─────────────────────────────────────────────────────────────────────────────

void GenIEBackend::terminateWorker(bool force) {
    if (current_is_vlm_.load()) {
        if (vlm_worker_) {
            vlm_worker_->terminateWorker(force);
        }
    } else {
        if (llm_worker_) {
            llm_worker_->terminateWorker(force);
        }
    }
    current_model_id_.clear();
    current_is_vlm_.store(false);
    runtime_artifacts_.reset();
}

bool GenIEBackend::forceKillActiveWorker() {
    if (current_is_vlm_.load()) {
        return vlm_worker_ && vlm_worker_->forceKillActiveWorker();
    }
    return llm_worker_ && llm_worker_->forceKillActiveWorker();
}

bool GenIEBackend::sendAbort(const std::string& session_id) {
    if (current_is_vlm_.load()) {
        return vlm_worker_ && vlm_worker_->sendAbort(session_id);
    }
    return llm_worker_ && llm_worker_->sendAbort(session_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// isHealthy()
// ─────────────────────────────────────────────────────────────────────────────

bool GenIEBackend::isHealthy() const {
    if (current_is_vlm_.load()) {
        return vlm_worker_ && vlm_worker_->isWorkerRunning();
    }
    return llm_worker_ && llm_worker_->isWorkerRunning();
}
