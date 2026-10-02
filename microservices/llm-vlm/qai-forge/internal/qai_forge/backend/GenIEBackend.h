// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#ifdef QAI_FORGE_BUILD_GENIE

#include "qai_forge/backend/IGenerativeBackend.h"
#include "qai_forge/backend/IPlacementAwareGenerativeBackend.h"
#include "qai_forge/execution/ExecutionPlan.h"
#include "qai_forge/execution/GenieConfigMaterializer.h"
#include <string>
#include <atomic>
#include <memory>

class InferenceWorkerManager;
class VlmInferenceWorkerManager;

// ─────────────────────────────────────────────────────────────────────────────
// GenIEBackend — qai_forge::IGenerativeBackend implementation for Qualcomm GenIE SDK
//
// Wraps InferenceWorkerManager (LLM) and VlmInferenceWorkerManager (VLM).
// Both LLM and VLM models with "runtime": "genie" in metadata.json use this
// backend. The LLM vs VLM distinction is an internal detail — the orchestrator
// only calls generate() or generateVlm() and never touches the workers directly.
//
// GenIE-specific details encapsulated here (invisible to ChatOrchestratorImpl):
//   - bypass_think_filter flag in the EXECUTE IPC command
//   - Worker selection: InferenceWorkerManager vs VlmInferenceWorkerManager
//   - KV cache reset via sendReset() in onContextCompacted()
//   - KV save/restore via GenieDialog_save() / GenieDialog_restore()
//
// The static worker singletons (getLlmWorker, getVlmWorker) that previously
// lived in ChatOrchestratorImpl.cpp have been moved here — worker lifecycle
// is a backend concern, not an orchestrator concern.
//
// Placement: loadModel(RuntimeLaunchRequest) materializes a runtime-specific
// GenIE config tree via GenieConfigMaterializer and patches devices[0].device_id
// per the resolved ExecutionPlan. runtime_artifacts_ keeps that tree alive
// until the worker is torn down.
// ─────────────────────────────────────────────────────────────────────────────

class GenIEBackend : public qai_forge::IGenerativeBackend,
                      public IPlacementAwareGenerativeBackend {
public:
    GenIEBackend();
    ~GenIEBackend() override;

    std::string name() const override { return "GenIE"; }

    /**
     * GenIE capabilities:
     *   - RESET_KV context strategy (stateful KV cache; reset after summarization)
     *   - Per-handle exclusive execution. One loaded GenIE worker serializes its
     *     own requests; separate scheduler-owned workers may submit concurrently.
     *   - backend_filters_think_tokens = true (GenIE SDK can filter internally)
     *   - supports_kv_save_restore = true for LLM; false for VLM
     *
     * context_window is read from ModelConfigManager for the current model.
     * Returns defaults if no model is currently loaded.
     */
    BackendCapabilities capabilities() const override;

    /**
     * Load a model into this GenIEBackend instance.
     * Resolves the model config through ModelConfigManager and starts the
     * matching LLM or VLM worker.
     */
    void loadModel(const std::string& model_id) override;

    void loadModel(
        const qai_forge::execution::RuntimeLaunchRequest& request) override;

    /**
     * Placement-aware overload for a default (replica_id=0) runtime identity.
     */
    void loadModel(
        const std::string& model_id,
        const qai_forge::execution::ExecutionPlan& plan);

    /**
     * Unload this backend instance's active worker.
     */
    void unloadModel(bool force = false) override;

    /**
     * Ensure the correct worker subprocess is running.
     * Selects LLM or VLM worker based on ModelConfigManager::supportsVision().
     * Updates current_model_id_ and current_is_vlm_ for subsequent calls.
     */
    void ensureWorkerRunning(const std::string& model_id,
                             const std::string& config_file,
                             const std::string& sampler_file) override;

    void ensureWorkerRunning(const std::string& model_id,
                             const std::string& config_file,
                             const std::string& sampler_file,
                             bool validate_ready);

    /**
     * Text-only LLM inference.
     * Maps use_reasoning → bypass_think_filter (GenIE SDK internal flag).
     *
     * The orchestrator decides WHETHER to use reasoning (model capability +
     * budget calculation). GenIEBackend decides HOW to implement it by setting
     * bypass_think_filter in the EXECUTE IPC command.
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
     * Vision-language VLM inference.
     * Delegates to VlmInferenceWorkerManager::executeVlmRequest().
     * images are preprocessed tensors from the orchestrator's
     * preprocessVision() adapter — copied into IPC shared memory and
     * referenced by the VLM worker, no temp files involved.
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
     * No-op: KV cache reset is now handled by resetKvAsync() which is called
     * by GenieOrchestrator immediately after every generate() / generateVlm()
     * call. Kept for interface compatibility with qai_forge::IGenerativeBackend.
     */
    void onContextCompacted() override;

    /**
     * Initiate an eager background KV cache reset after inference completes.
     *
     * Called by GenieOrchestrator immediately after generate() / generateVlm()
     * returns. The parent starts the reset asynchronously; the worker keeps
     * the completed session's handle reserved until RESET completes.
     *
     * The reset runs in a background thread so it overlaps with returning the
     * response to the HTTP layer. The scheduler waits for the reset before
     * releasing the session and concurrency reservation.
     */
    void resetKvAsync(const std::string& session_id = "") override;
    void waitForSessionReady(const std::string& session_id = "") override;

    /**
     * KV cache checkpoint operations (LLM only; VLM does not support these).
     * Delegates to InferenceWorkerManager::saveKvCache() / restoreKvCache().
     */
    void saveKv(const std::string& name, const std::string& session_id = "")    override;
    void restoreKv(const std::string& name, const std::string& session_id = "") override;
    void resetKv(const std::string& session_id = "")                           override;

    /**
     * Terminate the active worker subprocess.
     * @param force  true → SIGKILL (immediate, used for client cancellation)
     *               false → graceful SHUTDOWN command
     */
    void terminateWorker(bool force = false) override;
    bool forceKillActiveWorker() override;

    /**
     * Cancel a single session's in-flight generation. Delegates to
     * InferenceWorkerManager::sendAbort() (LLM) or
     * VlmInferenceWorkerManager::sendAbort() (VLM) — routes to whichever
     * worker is active per current_is_vlm_.
     */
    bool sendAbort(const std::string& session_id) override;

    /**
     * Returns true if the active worker subprocess is alive.
     */
    bool isHealthy() const override;

private:
    GenIEBackend(const GenIEBackend&) = delete;
    GenIEBackend& operator=(const GenIEBackend&) = delete;

    InferenceWorkerManager& llmWorker();
    VlmInferenceWorkerManager& vlmWorker();
    int effectiveMaxConcurrent(const std::string& model_id) const;

    void loadModel(
        const std::string& model_id,
        const qai_forge::execution::RuntimeId& runtime_id,
        const qai_forge::execution::ExecutionPlan& plan);

    // Track which worker is currently active.
    // Updated by ensureWorkerRunning() on each request.
    std::string current_model_id_;
    std::atomic<bool> current_is_vlm_{false};

    std::unique_ptr<InferenceWorkerManager>    llm_worker_;
    std::unique_ptr<VlmInferenceWorkerManager> vlm_worker_;

    // Retained until the corresponding worker has been terminated and
    // reaped. Keeps the runtime-specific materialized GenIE config tree
    // alive across inference and same-runtime worker restarts.
    std::shared_ptr<qai_forge::execution::RuntimeArtifacts> runtime_artifacts_;
};

#endif // QAI_FORGE_BUILD_GENIE
