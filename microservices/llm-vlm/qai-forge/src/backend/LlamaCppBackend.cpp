// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// -----------------------------------------------------------------------------
// LlamaCppBackend — qai_forge::IGenerativeBackend implementation wrapping the direct-
// libllama llamacpp-inference-worker subprocess (no llama-server, no HTTP).
// Mirrors LiteRTLMBackend's structure.
// -----------------------------------------------------------------------------

#ifdef QAI_FORGE_BUILD_LLAMACPP

#include "qai_forge/backend/LlamaCppBackend.h"
#include "qai_forge/execution/LlamaCppLaunchOptions.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/Logger.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// -----------------------------------------------------------------------------
// Construction
// -----------------------------------------------------------------------------

LlamaCppBackend::LlamaCppBackend() = default;

// -----------------------------------------------------------------------------
// worker() — lazy-initialize the LlamaCppWorkerManager
// -----------------------------------------------------------------------------

LlamaCppWorkerManager& LlamaCppBackend::worker() {
    if (!worker_) {
        worker_ = std::make_unique<LlamaCppWorkerManager>();
    }
    return *worker_;
}

// -----------------------------------------------------------------------------
// capabilities()
// -----------------------------------------------------------------------------

BackendCapabilities LlamaCppBackend::capabilities() const {
    auto& cfg = ModelConfigManager::getInstance();
    return BackendCapabilities{
        // MVP: full merged message history is re-rendered and re-decoded
        // every EXECUTE (KV cache cleared at the start of handleExecute()) —
        // no incremental KV reuse across turns yet.
        .context_strategy             = ContextStrategy::FULL_RECOMPUTE,
        .context_window               = current_model_id_.empty()
                                            ? 4096
                                            : cfg.getContextSize(current_model_id_),
        .compaction_threshold         = 0.70f,
        // One request at a time per worker (matches today's --parallel 1).
        .concurrency_model            = ConcurrencyModel::EXCLUSIVE,
        .max_concurrent               = 1,
        .backend_filters_think_tokens = false,
        // SAVE_KV/RESTORE_KV are stubbed (ack with READY, no-op) in the MVP worker.
        .supports_kv_save_restore     = false,
    };
}

void LlamaCppBackend::loadModel(
    const qai_forge::execution::RuntimeLaunchRequest& request) {
    loadModel(request.model_id, request.execution_plan);
}

void LlamaCppBackend::loadModel(
    const std::string& model_id,
    const qai_forge::execution::ExecutionPlan& plan) {
    // Resolves and validates the Hexagon endpoint selector encoded in the
    // plan (e.g. "HTP1:0"). Threading it into LlamaCppWorkerManager::startWorker()
    // is a Phase E worker-protocol change; for now this call validates the
    // plan and the legacy loadModel(model_id) path continues unchanged.
    (void)qai_forge::execution::LlamaCppLaunchOptions::toInitOptions(plan);
    loadModel(model_id);
}

// -----------------------------------------------------------------------------
// writeWorkerConfigFile() — synthesize the JSON sidecar the worker's
// loadModel() parses. ModelConfigManager's GGUF auto-discovery sets
// ModelConfig::config_file to the raw .gguf path directly (there is no
// processed-bundle step for bare .gguf files), so this backend has to
// produce the sidecar itself rather than read one written elsewhere.
// -----------------------------------------------------------------------------

std::string LlamaCppBackend::writeWorkerConfigFile(const std::string& model_id,
                                                    const std::string& gguf_path,
                                                    int context_size,
                                                    const std::string& mmproj_path) {
    const fs::path dir = "/tmp/configs";
    std::error_code ec;
    fs::create_directories(dir, ec);

    const fs::path sidecar_path = dir / (model_id + ".llamacpp.json");

    // Default matches the old llama-server invocation's "--n-gpu-layers 999"
    // (offload as many layers as the backend will accept). Overridable per
    // deployment via GENAI_LLAMACPP_NGL - some MoE GGUFs (e.g. 32-expert
    // models) pack enough weight into the layers assigned to a single HTP
    // device that llama.cpp's HTP*-REPACK buffer allocation exceeds the
    // Hexagon FastRPC/ION single-mapping limit (~1GiB) and fails to load;
    // lowering n_gpu_layers shrinks that per-device buffer by leaving some
    // layers on CPU.
    int n_gpu_layers = 999;
    const char* ngl_env = std::getenv("GENAI_LLAMACPP_NGL");
    if (ngl_env) {
        try {
            n_gpu_layers = std::stoi(ngl_env);
        } catch (...) {}
    }
    LOG_INFO("[LlamaCppBackend] writeWorkerConfigFile: model=" << model_id
             << " n_gpu_layers=" << n_gpu_layers
             << " (GENAI_LLAMACPP_NGL=" << (ngl_env ? ngl_env : "<unset>") << ")");

    json cfg = {
        {"model_path", gguf_path},
        {"context_size", context_size},
        {"n_gpu_layers", n_gpu_layers},
    };
    if (!mmproj_path.empty()) {
        cfg["mmproj_path"] = mmproj_path;
    }

    std::ofstream out(sidecar_path);
    if (!out) {
        throw std::runtime_error(
            "[LlamaCppBackend] Failed to write worker config file: " + sidecar_path.string());
    }
    out << cfg.dump(2);
    out.close();

    return sidecar_path.string();
}

// -----------------------------------------------------------------------------
// loadModel()
// -----------------------------------------------------------------------------

void LlamaCppBackend::loadModel(const std::string& model_id) {
    const auto model_config =
        ModelConfigManager::getInstance().getModelConfig(model_id);
    if (!model_config) {
        throw GenAIException(
            GenAIErrorCode::MODEL_NOT_FOUND,
            "Model '" + model_id + "' not found. Check /v1/models for available models.",
            404);
    }

    // model_config->config_file is the raw .gguf path for llamacpp models
    // (see ModelConfigManager's GGUF auto-discovery) — not the sidecar the
    // worker expects, so build the sidecar here.
    const std::string sidecar_path =
        writeWorkerConfigFile(model_id, model_config->config_file,
                              model_config->context_size, model_config->mmproj_path);

    LOG_INFO("[LlamaCppBackend] loadModel: model=" << model_id
             << " gguf=" << model_config->config_file
             << " config=" << sidecar_path
             << (model_config->mmproj_path.empty()
                     ? ""
                     : " mmproj=" + model_config->mmproj_path));

    ensureWorkerRunning(model_id, sidecar_path, "");
}

// -----------------------------------------------------------------------------
// unloadModel()
//
// force=false (normal drain/eviction): graceful shutdown() sends the
// SHUTDOWN IPC command first. The worker's own exit path then calls
// llama_free()/llama_model_free(), tearing down the ggml-hex HTP0/HTP1
// sessions and releasing their FastRPC/ION REPACK buffers. Skipping this
// (plain SIGKILL) was observed to leak Hexagon DSP memory across reload
// cycles for LFM2.5-8B-A1B's MoE GGUF: a later load with a SMALLER buffer
// request failed where an earlier larger one had succeeded. shutdown()
// still falls back to SIGKILL if the worker does not exit within its
// grace period.
//
// force=true (e.g. cancelSession()'s emergency teardown): keep immediate
// SIGKILL for a guaranteed-fast teardown.
// -----------------------------------------------------------------------------

void LlamaCppBackend::unloadModel(bool force) {
    LOG_INFO("[LlamaCppBackend] unloadModel: model=" << current_model_id_
             << " force=" << (force ? "true" : "false"));

    if (worker_) {
        if (force) {
            worker_->terminateWorker(true);
        } else {
            worker_->shutdown();
        }
        worker_.reset();
    }

    current_model_id_.clear();
}

// -----------------------------------------------------------------------------
// ensureWorkerRunning()
// -----------------------------------------------------------------------------

void LlamaCppBackend::ensureWorkerRunning(const std::string& model_id,
                                          const std::string& config_file,
                                          const std::string& sampler_file) {
    current_model_id_ = model_id;
    LOG_DEBUG("[LlamaCppBackend] ensureWorkerRunning: model=" << model_id
              << " config=" << config_file);
    worker().ensureWorkerRunning(model_id, config_file, sampler_file);
}

// -----------------------------------------------------------------------------
// generate() — legacy plain-text path, not used by LlamaCppOrchestrator
// -----------------------------------------------------------------------------

void LlamaCppBackend::generate(
    const std::string& event_id,
    const std::string& /*session_id*/,
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
    std::function<void(const IPCErrorEvent&)>  on_error) {

    // This method should not be used for llama.cpp
    // The orchestrator should use httpPostBlocking/Streaming directly
    (void)event_id; (void)prompt; (void)streaming; (void)max_tokens;
    (void)temperature; (void)top_p; (void)top_k; (void)presence_penalty;
    (void)frequency_penalty; (void)use_reasoning;
    (void)on_token; (void)on_done;

    LOG_ERROR("[LlamaCppBackend] generate() called - use LlamaCppOrchestrator instead");
    IPCErrorEvent error_event;
    error_event.event_id = event_id;
    error_event.message = "LlamaCppBackend::generate() not supported - use httpPostBlocking/Streaming";
    on_error(error_event);
}

// -----------------------------------------------------------------------------
// generateStructured() — structured chat inference
// -----------------------------------------------------------------------------

void LlamaCppBackend::generateStructured(
    const std::string& event_id,
    const std::string& session_id,
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
    std::function<void(const IPCErrorEvent&)> on_error,
    const std::vector<std::vector<uint8_t>>& images) {
    worker().executeStructuredRequest(
        event_id,
        session_id,
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
        std::move(on_error),
        images);
}

// -----------------------------------------------------------------------------
// KV cache operations — RESET is the only meaningful one (MVP: no save/restore)
// -----------------------------------------------------------------------------

void LlamaCppBackend::saveKv(const std::string& name, const std::string& /*session_id*/) {
    LOG_WARN("[LlamaCppBackend] saveKv('" << name << "') not supported (MVP)");
}

void LlamaCppBackend::restoreKv(const std::string& name, const std::string& /*session_id*/) {
    LOG_WARN("[LlamaCppBackend] restoreKv('" << name << "') not supported (MVP)");
}

void LlamaCppBackend::resetKv(const std::string& session_id) {
    try {
        worker().sendReset(session_id);
    } catch (const std::exception& e) {
        LOG_WARN("[LlamaCppBackend] resetKv failed: " << e.what());
    }
}

// -----------------------------------------------------------------------------
// terminateWorker() / forceKillActiveWorker()
// -----------------------------------------------------------------------------

void LlamaCppBackend::terminateWorker(bool force) {
    if (worker_) {
        worker_->terminateWorker(force);
    }
    current_model_id_.clear();
}

bool LlamaCppBackend::forceKillActiveWorker() {
    // Delegates to the worker manager's native support, fixing the old
    // HTTP-based backend's inability to cancel one in-flight request
    // without killing the whole worker.
    return worker_ && worker_->forceKillActiveWorker();
}

// -----------------------------------------------------------------------------
// isHealthy()
// -----------------------------------------------------------------------------

bool LlamaCppBackend::isHealthy() const {
    return worker_ && worker_->isWorkerRunning();
}

#endif // QAI_FORGE_BUILD_LLAMACPP
