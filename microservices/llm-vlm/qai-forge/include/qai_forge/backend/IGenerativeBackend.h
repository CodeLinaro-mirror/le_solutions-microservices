// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/backend/BackendCapabilities.h"
#include "qai_forge/worker/InferenceProtocol.h"
#include <string>
#include <vector>
#include <functional>
#include <stdexcept>

// ─────────────────────────────────────────────────────────────────────────────
// IGenerativeBackend — Layer 3 interface for generative AI inference
//
// This interface sits between ChatOrchestratorImpl (Layer 2) and the worker
// subprocess managers (Layer 3). It replaces the direct calls to
// InferenceWorkerManager / VlmInferenceWorkerManager that were previously
// embedded in ChatOrchestratorImpl.
//
// Current implementations:
//   GenIEBackend    — wraps InferenceWorkerManager (LLM) +
//                     VlmInferenceWorkerManager (VLM)
//
// Future implementations:
//   LiteRTLMBackend — wraps LiteRT LM (MediaPipe Tasks LlmInference)
//   OnnxRTBackend   — wraps ONNX Runtime encode/decode loop
//
// Design invariants (docs/genai-backend-decoupling.md):
//   - ChatOrchestratorImpl ONLY calls methods on this interface.
//   - bypass_think_filter is NOT in this interface. It is a GenIE SDK internal
//     detail handled inside GenIEBackend::generate().
//   - The orchestrator passes use_reasoning=true/false; the backend decides
//     how to implement it (bypass_think_filter for GenIE, raw output for others).
//   - generateVlm() has a default no-op body. Only backends that support
//     vision override it (currently GenIEBackend).
//   - onContextCompacted() has a default no-op body. Backends with stateful
//     KV caches (GenIE, LiteRT LM) override it to reset the cache.
// ─────────────────────────────────────────────────────────────────────────────

class IGenerativeBackend {
public:
    virtual ~IGenerativeBackend() = default;

    // Human-readable name for logging ("GenIE", "LiteRTLM", "OnnxRT")
    virtual std::string name() const = 0;

    /**
     * Declare this backend's capabilities.
     * Called by GenerativeOrchestrator at startup to build the middleware pipeline.
     * The returned struct reflects the currently loaded model's constraints.
     */
    virtual BackendCapabilities capabilities() const = 0;

    /**
     * Load a model into this backend instance.
     *
     * Scheduler-owned backends override this to turn a model_id into backend
     * config paths and start the appropriate worker. Legacy callers may still
     * use ensureWorkerRunning() directly.
     */
    virtual void loadModel(const std::string& model_id) {
        (void)model_id;
        throw std::runtime_error("Backend does not implement loadModel()");
    }

    /**
     * Unload the model owned by this backend instance.
     *
     * Default behavior preserves older backend implementations by terminating
     * the active worker through the existing interface.
     */
    virtual void unloadModel(bool force = false) {
        terminateWorker(force);
    }

    /**
     * Ensure the worker subprocess is running with the correct model loaded.
     * If the model has changed, the old worker is killed and a new one started.
     * If the worker has crashed, a new one is started automatically.
     *
     * @param model_id     Model identifier (e.g. "qwen3-8b")
     * @param config_file  Absolute path to the processed config (genie_config.json)
     * @param sampler_file Absolute path to the sampler config
     */
    virtual void ensureWorkerRunning(const std::string& model_id,
                                     const std::string& config_file,
                                     const std::string& sampler_file) = 0;

    /**
     * Run text-only (LLM) inference.
     *
     * @param event_id       Unique ID for this inference event
     * @param prompt         Compacted context prompt assembled by Layer 2
     * @param streaming      Whether to stream tokens or accumulate
     * @param max_tokens     Max completion tokens
     * @param temperature    Sampling temperature
     * @param top_p          Top-p sampling
     * @param top_k          Top-k sampling
     * @param presence_penalty   Presence penalty
     * @param frequency_penalty  Frequency penalty
     * @param use_reasoning  Orchestrator sets this based on model capabilities +
     *                       reasoning budget calculation.
     *                       GenIEBackend maps this to bypass_think_filter internally.
     *                       Other backends always output raw tokens regardless.
     * @param on_token       Called for each TOKEN event from the worker
     * @param on_done        Called when DONE event received
     * @param on_error       Called on ERROR event or socket failure
     */
    virtual void generate(
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
        std::function<void(const IPCErrorEvent&)>  on_error) = 0;

    /**
     * Run vision-language (VLM) inference.
     *
     * Default implementation is a no-op — only backends that support VLM
     * override this (currently GenIEBackend).
     *
     * @param image_paths  Preprocessed image file paths written by the
     *                     orchestrator's preprocessImagesToTempFiles().
     *                     The backend loads these files and passes them to
     *                     the VLM pipeline. The orchestrator owns the temp
     *                     files and deletes them via TempFileGuard.
     */
    virtual void generateVlm(
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
        std::function<void(const IPCErrorEvent&)>  on_error) {
        // Default: no-op. Backends without VLM support do not override this.
        (void)event_id; (void)prompt; (void)image_paths; (void)streaming;
        (void)max_tokens; (void)temperature; (void)top_p; (void)top_k;
        (void)presence_penalty; (void)frequency_penalty;
        (void)on_token; (void)on_done; (void)on_error;
    }

    /**
     * Called by SummarizationMiddleware immediately after summarization completes.
     *
     * GenIEBackend:    calls sendReset() on the active worker to clear the KV
     *                  cache. The next turn re-feeds the compacted context.
     * LiteRTLMBackend: calls LlmInference::ResetContext() (future).
     * OnnxRTBackend:   no-op — OnnxRT recomputes full context each turn anyway.
     *
     * Default implementation is a no-op.
     */
    virtual void onContextCompacted() {}

    /**
     * KV cache checkpoint operations.
     * Only meaningful when capabilities().supports_kv_save_restore == true.
     * Used by reasoning budget management to save/restore KV state around
     * <think> blocks for precise budget enforcement.
     * Default implementations are no-ops.
     */
    virtual void saveKv(const std::string& name)    { (void)name; }
    virtual void restoreKv(const std::string& name) { (void)name; }
    virtual void resetKv()                          {}

    /**
     * Terminate the worker subprocess.
     * @param force  If true, sends SIGKILL immediately (used for client
     *               cancellation via cancelSession()).
     *               If false, sends graceful SHUTDOWN command.
     */
    virtual void terminateWorker(bool force = false) = 0;

    /**
     * Returns true if the worker subprocess is alive and responsive.
     */
    virtual bool isHealthy() const = 0;
};
