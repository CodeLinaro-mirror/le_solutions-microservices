// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/backend/IGenerativeBackend.h"
#include "qai_forge/worker/LiteRTLMWorkerManager.h"
#include <memory>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// LiteRTLMBackend — Phase 3: IGenerativeBackend for LiteRT-LM
//
// Wraps LiteRTLMWorkerManager to manage the litert-lm-inference-worker
// subprocess. Supports .litertlm models with NPU acceleration via the
// LiteRT-LM Session API.
//
// Key design decisions:
//   - Uses LiteRTLMWorkerManager (extends InferenceWorkerManager) to intercept
//     the METADATA message sent by the worker after model load.
//   - Exposes getMetadata() so LiteRTLMOrchestrator can retrieve the Jinja
//     template, model type, and tool call delimiters at construction time.
//   - unloadModel() always uses SIGKILL (force=true) to ensure clean DSP
//     resource reclamation — same pattern as GenIEBackend::unloadModel().
//   - Scheduler-owned: each ModelRuntime creates its own LiteRTLMBackend
//     instance via BackendFactory::createGenerativeBackend("litert_lm").
//
// metadata.json fields used:
//   "runtime":    "litert_lm"
//   "model_type": "generative"
//   The config_file path points to the .litertlm model bundle.
//
// Environment variable overrides:
//   LITERT_LM_WORKER_BINARY — path to litert-lm-inference-worker binary
//   LITERT_LM_NUM_THREADS   — number of inference threads (default 4)
// ─────────────────────────────────────────────────────────────────────────────

class LiteRTLMBackend : public IGenerativeBackend {
public:
    /**
     * Public constructor — creates an owned (non-singleton) instance.
     * Used by BackendFactory::createGenerativeBackend("litert_lm") for
     * scheduler-owned instances. Each ModelRuntime owns its own LiteRTLMBackend.
     */
    LiteRTLMBackend();

    /**
     * Legacy singleton accessor — kept for backward compatibility with
     * BackendFactory::getGenerativeBackend("litert_lm").
     * New code should use BackendFactory::createGenerativeBackend("litert_lm").
     */
    static LiteRTLMBackend& getInstance();

    std::string name() const override { return "LiteRTLM"; }

    /**
     * LiteRT-LM capabilities:
     *   - RESET_KV context strategy (stateful KV cache; reset after compaction)
     *   - Exclusive execution (one inference at a time per NPU handle)
     *   - backend_filters_think_tokens = false (no internal think filtering)
     *   - supports_kv_save_restore = false (not yet implemented)
     */
    BackendCapabilities capabilities() const override;

    /**
     * Load a model into this backend instance.
     * Resolves the model config through ModelConfigManager and starts the
     * litert-lm-inference-worker subprocess.
     */
    void loadModel(const std::string& model_id) override;

    /**
     * Unload the model and terminate the worker subprocess.
     * Always uses SIGKILL (force=true) to ensure clean DSP resource reclamation.
     */
    void unloadModel(bool force = false) override;

    /**
     * Ensure the litert-lm-inference-worker is running with the given model.
     * Delegates to LiteRTLMWorkerManager::ensureWorkerRunning() which also
     * intercepts the METADATA message from the worker.
     */
    void ensureWorkerRunning(const std::string& model_id,
                             const std::string& config_file,
                             const std::string& sampler_config) override;

    /**
     * Text-only LLM inference.
     * Delegates to LiteRTLMWorkerManager::executeRequest().
     * use_reasoning is ignored — LiteRT-LM does not filter <think> tokens.
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
     * VLM inference — not supported by LiteRT-LM (LLM-only backend).
     * Calls on_error immediately.
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
     * Called after context compaction — resets the KV cache.
     * Delegates to LiteRTLMWorkerManager::sendReset().
     */
    void onContextCompacted() override;

    /**
     * Initiate an eager background KV cache reset after inference completes.
     * Delegates to LiteRTLMWorkerManager::initiateBackgroundReset().
     */
    void resetKvAsync() override;

    /**
     * KV cache operations — not yet supported for LiteRT-LM.
     * saveKv/restoreKv are no-ops; resetKv calls sendReset().
     */
    void saveKv(const std::string& name)    override;
    void restoreKv(const std::string& name) override;
    void resetKv()                          override;

    /**
     * Terminate the worker subprocess.
     * @param force  true → SIGKILL (immediate, used for client cancellation)
     *               false → graceful SHUTDOWN command
     */
    void terminateWorker(bool force = false) override;
    bool forceKillActiveWorker() override;

    /**
     * Returns true if the worker subprocess is alive.
     */
    bool isHealthy() const override;

    /**
     * Return the metadata received from the worker after model load.
     * Used by LiteRTLMOrchestrator to initialize prompt templating and
     * tool call parsing.
     */
    const LiteRTLMMetadata& getMetadata() const;

private:
    LiteRTLMBackend(const LiteRTLMBackend&)            = delete;
    LiteRTLMBackend& operator=(const LiteRTLMBackend&) = delete;

    LiteRTLMWorkerManager& worker();

    std::string current_model_id_;
    std::unique_ptr<LiteRTLMWorkerManager> worker_;
};
