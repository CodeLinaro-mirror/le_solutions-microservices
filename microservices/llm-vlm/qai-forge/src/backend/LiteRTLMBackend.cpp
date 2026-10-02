// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// LiteRTLMBackend — qai_forge::IGenerativeBackend implementation for LiteRT-LM
//
// Wraps LiteRTLMWorkerManager (which extends InferenceWorkerManager) to manage
// the litert-lm-inference-worker subprocess. The worker uses the LiteRT-LM
// Session API for raw inference; all chat intelligence lives in
// LiteRTLMOrchestrator.
//
// Notes:
//   - Exposes getMetadata() for LiteRTLMOrchestrator initialization
//   - unloadModel() always uses SIGKILL for clean DSP resource reclamation
//   - Supports scheduler-owned instances (one per ModelRuntime)
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/backend/LiteRTLMBackend.h"
#include "qai_forge/execution/LiteRtLmLaunchOptions.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/Logger.h"

#include <utility>

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

LiteRTLMBackend::LiteRTLMBackend() = default;

// ─────────────────────────────────────────────────────────────────────────────
// worker() — lazy-initialize the LiteRTLMWorkerManager
// ─────────────────────────────────────────────────────────────────────────────

LiteRTLMWorkerManager& LiteRTLMBackend::worker() {
    if (!worker_) {
        worker_ = std::make_unique<LiteRTLMWorkerManager>();
    }
    return *worker_;
}

LiteRTLMBackend::~LiteRTLMBackend() = default;

// ─────────────────────────────────────────────────────────────────────────────
// capabilities()
// ─────────────────────────────────────────────────────────────────────────────

BackendCapabilities LiteRTLMBackend::capabilities() const {
    auto& cfg = ModelConfigManager::getInstance();
    return BackendCapabilities{
        // LiteRT-LM uses per-request sessions (stateless).
        // FULL_RECOMPUTE tells the scheduler NOT to call resetKv() after each inference.
        .context_strategy             = ContextStrategy::FULL_RECOMPUTE,
        .context_window               = current_model_id_.empty()
                                            ? []() -> int {
                                                const char* env = std::getenv("LITERT_LM_DEFAULT_CONTEXT_LENGTH");
                                                return (env && std::atoi(env) > 0) ? std::atoi(env) : 4096;
                                              }()
                                            : cfg.getContextSize(current_model_id_),
        .compaction_threshold         = []() -> float {
            const char* env = std::getenv("LITERT_LM_COMPACTION_THRESHOLD");
            if (env) { float v = std::stof(env); if (v > 0.0f && v < 1.0f) return v; }
            return 0.75f;
        }(),
        // Qualcomm NPU: only one inference at a time per backend instance
        .concurrency_model            = ConcurrencyModel::EXCLUSIVE,
        .max_concurrent               = 1,
        // LiteRT-LM does not filter <think> tokens internally
        .backend_filters_think_tokens = false,
        // KV save/restore not yet supported for LiteRT-LM
        .supports_kv_save_restore     = false,
        // LiteRT-LM maintains per-session KV state across requests via the
        // worker's g_kv_sessions map — generateWithSession() is meaningful.
        .supports_kv_session_reuse    = true,
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// loadModel()
// ─────────────────────────────────────────────────────────────────────────────

void LiteRTLMBackend::loadModel(
    const qai_forge::execution::RuntimeLaunchRequest& request) {
    loadModel(request.model_id, request.execution_plan);
}

void LiteRTLMBackend::loadModel(const std::string& model_id) {
    qai_forge::execution::ExecutionPlan automatic_plan;
    automatic_plan.provider = "litert_lm";
    qai_forge::execution::ComponentPlacement component;
    component.target.provider = "litert_lm";
    component.target.semantics =
        qai_forge::execution::TargetSemantics::BackendAutomatic;
    component.target.discovery_mode =
        qai_forge::execution::DiscoveryMode::BackendManaged;
    component.target.binding_strength =
        qai_forge::execution::BindingStrength::Automatic;
    automatic_plan.components.push_back(std::move(component));
    loadModel(model_id, automatic_plan);
}

void LiteRTLMBackend::loadModel(
    const std::string& model_id,
    const qai_forge::execution::ExecutionPlan& plan) {
    const auto model_config =
        ModelConfigManager::getInstance().getModelConfig(model_id);
    if (!model_config) {
        throw GenAIException(
            GenAIErrorCode::MODEL_NOT_FOUND,
            "Model '" + model_id + "' not found. Check /v1/models for available models.",
            404);
    }

    LOG_INFO("[LiteRTLMBackend] loadModel: model=" << model_id
             << " config=" << model_config->config_file);

    // toInitOptions() yields the optional {"litert_lm":{"backend":"cpu"|
    // "gpu"|"npu"}} override (present only when the plan specifies a
    // backend-class selector); setInitOptions() merges it into the next
    // INIT the worker manager sends. The worker (src/worker/litert-lm/
    // main.cpp) applies it by setenv()ing LITERT_LM_BACKEND before
    // loadModel(), falling back to the existing env/default otherwise.
    const auto launch_options =
        qai_forge::execution::LiteRtLmLaunchOptions::toInitOptions(plan);
    worker().setInitOptions(launch_options);
    // ready_validator runs on the worker's post-INIT READY and confirms
    // provider=="litert_lm" and binding_strength=="automatic" — LiteRT-LM
    // cannot confirm a physical target, so binding is always automatic.
    worker().ensureWorkerRunning(
        model_id, model_config->config_file, "",
        [](const json& ready) {
            if (ready.value("provider", "") != "litert_lm") {
                throw std::runtime_error(
                    "LiteRTLMBackend: worker READY reported an unexpected "
                    "provider");
            }
            if (ready.value("binding_strength", "") != "automatic") {
                throw std::runtime_error(
                    "LiteRTLMBackend: LiteRT-LM worker must report automatic "
                    "binding");
            }
        });

    current_model_id_ = model_id;
    current_plan_ = plan;
    has_current_plan_ = true;
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
    current_plan_ = {};
    has_current_plan_ = false;
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
    const std::string& session_id,
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
        event_id, session_id, prompt, streaming,
        max_tokens, temperature, top_p, top_k,
        presence_penalty, frequency_penalty,
        false,
        on_token, on_done, on_error);
}

void LiteRTLMBackend::generateWithSession(
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
    bool               /*use_reasoning*/,
    std::function<void(const IPCTokenEvent&)>  on_token,
    std::function<void(const IPCDoneEvent&)>   on_done,
    std::function<void(const IPCErrorEvent&)>  on_error,
    bool               kv_invalidated)
{
    worker().executeRequest(
        event_id, session_id, prompt, streaming,
        max_tokens, temperature, top_p, top_k,
        presence_penalty, frequency_penalty,
        false,
        on_token, on_done, on_error,
        kv_invalidated);
}

// ─────────────────────────────────────────────────────────────────────────────
// generateVlm() — not supported by LiteRT-LM (LLM-only backend)
// ─────────────────────────────────────────────────────────────────────────────

void LiteRTLMBackend::generateVlm(
    const std::string&              event_id,
    const std::string&              /*session_id*/,
    const std::string&              /*prompt*/,
    const std::vector<std::vector<uint8_t>>& /*images*/,
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
    on_error({event_id, "", "", "LiteRTLMBackend does not support VLM inference"});
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

void LiteRTLMBackend::resetKvAsync(const std::string& session_id) {
    // Demoted to DEBUG — see GenIEBackend::resetKvAsync()'s identical
    // rationale: this fires unconditionally after every inference call and
    // carries no diagnostic value at INFO beyond confirming the call
    // happened. Failures remain at WARN below.
    LOG_DEBUG("[LiteRTLMBackend] Initiating background KV reset for model: "
             << current_model_id_);
    try {
        if (worker_) {
            worker_->initiateBackgroundReset(session_id);
        }
    } catch (const std::exception& e) {
        LOG_WARN("[LiteRTLMBackend] Failed to initiate background KV reset: " << e.what()
                 << " (non-fatal)");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// KV cache operations
// ─────────────────────────────────────────────────────────────────────────────

void LiteRTLMBackend::saveKv(const std::string& name, const std::string& /*session_id*/) {
    // saveKv/restoreKv are not implemented for LiteRT-LM.
    // The LiteRT-LM C API (engine.h) does not expose SessionAdvanced::SaveCheckpoint /
    // RewindToCheckpoint. Cross-request KV cache is instead provided by keeping
    // LiteRtLmSession* alive in the worker's g_kv_sessions map with incremental prefill.
    // TODO: refactor to use saveKv/restoreKv once the SDK exposes these as C API —
    //       that would also enable sharing a system-prompt checkpoint across sessions.
    (void)name;
}

void LiteRTLMBackend::restoreKv(const std::string& name, const std::string& /*session_id*/) {
    (void)name;
}

void LiteRTLMBackend::resetKv(const std::string& session_id) {
    try {
        worker().sendReset(session_id);
    } catch (const std::exception& e) {
        LOG_WARN("[LiteRTLMBackend] resetKv failed: " << e.what());
    }
}

void LiteRTLMBackend::clearSession(const std::string& session_id) {
    if (worker_) {
        try {
            worker_->sendClearSession(session_id);
        } catch (const std::exception& e) {
            LOG_WARN("[LiteRTLMBackend] clearSession failed: " << e.what());
        }
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
    current_plan_ = {};
    has_current_plan_ = false;
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
