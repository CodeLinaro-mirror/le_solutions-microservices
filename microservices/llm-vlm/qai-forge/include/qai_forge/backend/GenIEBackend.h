// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/backend/IGenerativeBackend.h"
#include <string>
#include <atomic>
#include <memory>

class InferenceWorkerManager;
class VlmInferenceWorkerManager;

// ─────────────────────────────────────────────────────────────────────────────
// GenIEBackend — IGenerativeBackend implementation for Qualcomm GenIE SDK
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
// See docs/genai-backend-decoupling.md for the full design rationale.
// ─────────────────────────────────────────────────────────────────────────────

class GenIEBackend : public IGenerativeBackend {
public:
    /**
     * Returns the singleton GenIEBackend instance.
     * Thread-safe via Meyers singleton (C++11 guarantee).
     */
    static GenIEBackend& getInstance();

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
     * image_paths are preprocessed .raw files written by the orchestrator's
     * preprocessImagesToTempFiles() — the VLM worker loads them directly.
     */
    void generateVlm(
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
        std::function<void(const IPCErrorEvent&)>  on_error) override;

    /**
     * No-op: KV cache reset is now handled by resetKvAsync() which is called
     * by GenieOrchestrator immediately after every generate() / generateVlm()
     * call. Kept for interface compatibility with IGenerativeBackend.
     */
    void onContextCompacted() override;

    /**
     * Initiate an eager background KV cache reset after inference completes.
     *
     * Called by GenieOrchestrator immediately after generate() / generateVlm()
     * returns. Delegates to InferenceWorkerManager::initiateBackgroundReset()
     * (LLM) or VlmInferenceWorkerManager::initiateBackgroundReset() (VLM).
     *
     * The reset runs in a background thread so it overlaps with returning the
     * response to the HTTP layer. The next generate() call waits for the reset
     * to complete via waitForPendingReset() inside executeRequest().
     */
    void resetKvAsync() override;

    /**
     * KV cache checkpoint operations (LLM only; VLM does not support these).
     * Delegates to InferenceWorkerManager::saveKvCache() / restoreKvCache().
     */
    void saveKv(const std::string& name)    override;
    void restoreKv(const std::string& name) override;
    void resetKv()                          override;

    /**
     * Terminate the active worker subprocess.
     * @param force  true → SIGKILL (immediate, used for client cancellation)
     *               false → graceful SHUTDOWN command
     */
    void terminateWorker(bool force = false) override;

    /**
     * Returns true if the active worker subprocess is alive.
     */
    bool isHealthy() const override;

private:
    GenIEBackend(const GenIEBackend&) = delete;
    GenIEBackend& operator=(const GenIEBackend&) = delete;

    InferenceWorkerManager& llmWorker();
    VlmInferenceWorkerManager& vlmWorker();

    // Track which worker is currently active.
    // Updated by ensureWorkerRunning() on each request.
    std::string current_model_id_;
    bool        current_is_vlm_ = false;

    std::unique_ptr<InferenceWorkerManager>    llm_worker_;
    std::unique_ptr<VlmInferenceWorkerManager> vlm_worker_;
};
