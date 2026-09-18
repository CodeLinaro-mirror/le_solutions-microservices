// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#ifdef QAI_FORGE_BUILD_LLAMACPP

#include "qai_forge/backend/IGenerativeBackend.h"
#include "qai_forge/execution/ExecutionPlan.h"
#include "qai_forge/worker/LlamaCppWorkerManager.h"
#include <nlohmann/json.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <functional>
#include <vector>

using json = nlohmann::ordered_json;

namespace qai_forge {

// -----------------------------------------------------------------------------
// LlamaCppBackend � IGenerativeBackend for llama.cpp (direct libllama worker)
//
// Wraps LlamaCppWorkerManager to manage the llamacpp-inference-worker
// subprocess, which links libllama directly (no llama-server, no HTTP/SSE).
//
// Key design decisions:
//   - loadModel() writes a small JSON sidecar file {model_path, context_size,
//     n_gpu_layers, mmproj_path} to /tmp/configs � ModelConfigManager's GGUF
//     auto-discovery sets ModelConfig::config_file to the raw .gguf path
//     directly (there is no processed-bundle step for bare .gguf files the
//     way there is for GenIE bundles), so the sidecar has to be synthesized
//     here rather than read from an existing processed config.
//   - unloadModel() always uses SIGKILL (force=true), matching GenIEBackend/
//     LiteRTLMBackend's rationale for clean resource reclamation.
//   - forceKillActiveWorker() delegates to the worker manager's native
//     support � fixes the old HTTP-based backend's inability to cancel a
//     single in-flight request without killing the whole worker.
//   - generateVlm() is left as the IGenerativeBackend default no-op � vision
//     support is reached through generateStructured()'s images parameter
//     instead (mirrors how this backend already bypasses generate() for
//     structured/messages-based requests), since llamacpp requests are
//     always structured, never plain-text prompts.
//
// Environment variable overrides:
//   LLAMACPP_WORKER_BINARY � path to llamacpp-inference-worker binary
//                            (default /usr/local/bin/llamacpp-inference-worker)
// -----------------------------------------------------------------------------

class LlamaCppBackend : public IGenerativeBackend {
public:
    LlamaCppBackend();
    ~LlamaCppBackend() override = default;

    std::string name() const override { return "LlamaCpp"; }
    BackendCapabilities capabilities() const override;

    void loadModel(const std::string& model_id) override;
    void loadModel(
        const qai_forge::execution::RuntimeLaunchRequest& request) override;

    /**
     * Placement-aware overload. llama.cpp Hexagon endpoint selection
     * (GGML_HEXAGON_DEVICES / HTPN:0 expressions) is resolved via
     * LlamaCppLaunchOptions; wiring it into the worker's startWorker()
     * signature is a Phase E worker-protocol change.
     */
    void loadModel(
        const std::string& model_id,
        const qai_forge::execution::ExecutionPlan& plan);

    void unloadModel(bool force = false) override;

    void ensureWorkerRunning(const std::string& model_id,
                            const std::string& config_file,
                            const std::string& sampler_file) override;

    void generate(
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
        std::function<void(const IPCErrorEvent&)>  on_error) override;

    /**
     * Structured chat inference (messages/tools rendered via the model's own
     * embedded chat template inside the worker). Reached by
     * LlamaCppOrchestrator via dynamic_cast, mirroring
     * LiteRTLMBackend::generateStructured().
     *
     * @param images  Raw image bytes (e.g. PNG/JPEG), forwarded unmodified
     *                to the worker via the shared-memory image_refs
     *                mechanism. Empty for text-only requests or when the
     *                loaded model has no mmproj projector � the worker
     *                degrades gracefully to text-only in the latter case.
     */
    void generateStructured(
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
        const std::vector<std::vector<uint8_t>>& images = {});

    /**
     * Called after context compaction. FULL_RECOMPUTE already clears the KV
     * cache on every EXECUTE, so this is a no-op � kept for interface parity.
     */
    void onContextCompacted() override {}

    void saveKv(const std::string& name, const std::string& session_id = "")    override;
    void restoreKv(const std::string& name, const std::string& session_id = "") override;
    void resetKv(const std::string& session_id = "")                            override;

    void terminateWorker(bool force = false) override;
    bool forceKillActiveWorker() override;

    bool isHealthy() const override;

private:
    LlamaCppBackend(const LlamaCppBackend&)            = delete;
    LlamaCppBackend& operator=(const LlamaCppBackend&) = delete;

    LlamaCppWorkerManager& worker();

    /**
     * Write the {model_path, context_size, n_gpu_layers, mmproj_path} JSON
     * sidecar the worker's loadModel() parses, to
     * /tmp/configs/<model_id>.llamacpp.json.
     * @param gguf_path    Absolute path to the .gguf file (ModelConfig::config_file).
     * @param mmproj_path  Absolute path to the paired mmproj GGUF, or empty
     *                     if the model has no vision projector.
     * @return Absolute path to the written sidecar file.
     */
    std::string writeWorkerConfigFile(const std::string& model_id,
                                      const std::string& gguf_path,
                                      int context_size,
                                      const std::string& mmproj_path);

    std::string current_model_id_;
    std::unique_ptr<LlamaCppWorkerManager> worker_;
};

} // namespace qai_forge

#endif // QAI_FORGE_BUILD_LLAMACPP
