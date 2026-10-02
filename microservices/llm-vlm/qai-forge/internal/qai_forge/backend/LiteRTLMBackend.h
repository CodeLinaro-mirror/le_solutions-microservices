// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/backend/IGenerativeBackend.h"
#include "qai_forge/backend/IPlacementAwareGenerativeBackend.h"
#include "qai_forge/execution/ExecutionPlan.h"
#include "qai_forge/worker/LiteRTLMWorkerManager.h"
#include <memory>
#include <string>

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
//
// Placement: loadModel(RuntimeLaunchRequest) resolves a worker-facing
// backend-class selector (backend=npu|cpu|gpu) via LiteRtLmLaunchOptions and
// passes it to the worker through InferenceWorkerManager::setInitOptions(),
// merged into the next INIT command.
// ─────────────────────────────────────────────────────────────────────────────

class LiteRTLMBackend : public qai_forge::IGenerativeBackend,
                         public IPlacementAwareGenerativeBackend {
public:
    /**
     * Public constructor — creates an owned (non-singleton) instance.
     * Used by BackendFactory::createGenerativeBackend("litert_lm") for
     * scheduler-owned instances. Each ModelRuntime owns its own LiteRTLMBackend.
     */
    LiteRTLMBackend();
    ~LiteRTLMBackend() override;

    std::string name() const override { return "LiteRTLM"; }

    /**
     * LiteRT-LM capabilities:
     *   - FULL_RECOMPUTE context strategy (per-request Session, no persistent KV)
     *   - EXCLUSIVE concurrency (one inference at a time per backend instance)
     *   - backend_filters_think_tokens = false (raw output, filtering happens
     *     in LiteRTLMOrchestrator via ReasoningRouter like GenIE)
     */
    BackendCapabilities capabilities() const override;

    /**
     * Load a model into this LiteRTLMBackend instance.
     * Starts (or restarts, if the model changed) the
     * litert-lm-inference-worker subprocess.
     */
    void loadModel(const std::string& model_id) override;
    void loadModel(
        const qai_forge::execution::RuntimeLaunchRequest& request) override;

    /**
     * Placement-aware overload used when a caller already has an
     * ExecutionPlan for a default (replica_id=0) runtime identity.
     */
    void loadModel(
        const std::string& model_id,
        const qai_forge::execution::ExecutionPlan& plan);

    /**
     * Unload the model and terminate the worker subprocess.
     */
    void unloadModel(bool force = false) override;

    /**
     * Ensure the litert-lm-inference-worker is running with the given model.
     */
    void ensureWorkerRunning(const std::string& model_id,
                             const std::string& config_file,
                             const std::string& sampler_file) override;

    /**
     * Text-only LLM inference.
     */
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
     * Text-only LLM inference with cross-request KV session reuse.
     */
    void generateWithSession(
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
        std::function<void(const IPCErrorEvent&)>  on_error,
        bool               kv_invalidated = false) override;

    /**
     * Not supported by LiteRT-LM (LLM-only backend).
     */
    void generateVlm(
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
        std::function<void(const IPCErrorEvent&)>  on_error) override;

    /**
     * Resets the worker's KV cache after context compaction.
     */
    void onContextCompacted() override;

    void resetKvAsync(const std::string& session_id = "") override;

    /**
     * KV cache checkpoint operations — not implemented for LiteRT-LM.
     */
    void saveKv(const std::string& name, const std::string& session_id = "")    override;
    void restoreKv(const std::string& name, const std::string& session_id = "") override;
    void resetKv(const std::string& session_id = "")                           override;

    void clearSession(const std::string& session_id) override;

    /**
     * Terminate the worker subprocess.
     */
    void terminateWorker(bool force = false) override;
    bool forceKillActiveWorker() override;

    /**
     * Returns true if the worker subprocess is alive.
     */
    bool isHealthy() const override;

    /**
     * Return metadata received from worker after model load (jinja template,
     * tool call delimiters, thinking tags, etc.) for LiteRTLMOrchestrator.
     */
    const LiteRTLMMetadata& getMetadata() const;

private:
    LiteRTLMBackend(const LiteRTLMBackend&) = delete;
    LiteRTLMBackend& operator=(const LiteRTLMBackend&) = delete;

    LiteRTLMWorkerManager& worker();

    std::string current_model_id_;
    std::unique_ptr<LiteRTLMWorkerManager> worker_;
    qai_forge::execution::ExecutionPlan current_plan_;
    bool has_current_plan_ = false;
};
