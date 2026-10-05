// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/backend/BackendCapabilities.h"
#include "qai_forge/worker/InferenceEvents.h"
#include <string>
#include <vector>
#include <functional>
#include <stdexcept>
#include <cstdint>

namespace qai_forge {

// ─────────────────────────────────────────────────────────────────────────────
// qai_forge::IGenerativeBackend — public, out-of-tree ("Level B") generative
// backend ABI.
//
// This is the contract a third party implements to plug a new generative
// inference backend into qai-forge without access to qai-forge's source tree
// or internal scheduler plumbing. It sits between a
// qai_forge::IGenerativeOrchestrator implementation (Layer 2) and whatever
// worker subprocess / SDK call the backend wraps (Layer 3).
//
// NOTE on history: this interface was promoted from an internal,
// global-namespace ::IGenerativeBackend (internal/qai_forge/backend/
// IGenerativeBackend.h, now deleted) — same contract, minus the
// placement-aware loadModel(RuntimeLaunchRequest) overload, which remains an
// internal-only concern (see internal/qai_forge/backend/
// IPlacementAwareGenerativeBackend.h — the device-placement subsystem is a
// separate, not-yet-public ABI). internal/qai_forge/backend/BackendAdapter.h
// wraps any registered qai_forge::IGenerativeBackend and adds back
// placement-aware loading via dynamic_cast to that internal mixin, with a
// graceful fallback to loadModel(model_id) for backends (including
// out-of-tree third-party ones) that don't implement it.
//
// This is a deliberate, currently-permanent boundary of this public ABI,
// not an oversight: a third-party backend implementing ONLY this public
// interface can never participate in device placement, because the
// placement subsystem's own types (ExecutionPlan/RuntimeId/ResourceClaim)
// are still evolving and are not frozen into a public contract. See
// IPlacementAwareGenerativeBackend.h for the full rationale and the path
// forward if/when placement is eventually promoted to a public ABI.
//
// Current implementations (all internal, see internal/qai_forge/backend/):
//   GenIEBackend     — wraps InferenceWorkerManager (LLM) +
//                      VlmInferenceWorkerManager (VLM)
//   LiteRTLMBackend  — wraps LiteRT LM (MediaPipe Tasks LlmInference)
//   LlamaCppBackend  — wraps a direct-libllama worker subprocess
//
// Design invariants:
//   - The orchestrator ONLY calls methods on this interface.
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

    // Human-readable name for logging ("GenIE", "LiteRTLM", "LlamaCpp")
    virtual std::string name() const = 0;

    /**
     * Declare this backend's capabilities.
     * Called by the orchestrator at startup to build the middleware pipeline.
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
        std::function<void(const IPCErrorEvent&)>  on_error) = 0;

    /**
     * Run text-only (LLM) inference with cross-request KV session reuse.
     *
     * Only meaningful when capabilities().supports_kv_session_reuse == true.
     * Callers (orchestrators) MUST check this capability flag before calling
     * this method — backends that don't support session-based KV reuse throw
     * std::runtime_error from the default implementation below. This keeps
     * session_id/kv_invalidated out of the shared generate() signature that
     * every backend must implement, since only some backends currently have
     * a meaningful use for them.
     *
     * @param session_id      Stable session identifier used by the backend to
     *                        key its cross-request KV cache (e.g. worker-side
     *                        g_kv_sessions map).
     * @param kv_invalidated  true if context eviction occurred since the last
     *                        request in this session — the backend should
     *                        rebuild its KV state from scratch rather than
     *                        incrementally reusing the cached session state.
     */
    virtual void generateWithSession(
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
        bool               kv_invalidated = false) {
        (void)session_id; (void)streaming; (void)max_tokens; (void)temperature;
        (void)top_p; (void)top_k; (void)presence_penalty; (void)frequency_penalty;
        (void)use_reasoning; (void)on_token; (void)on_done; (void)kv_invalidated;
        (void)prompt;
        on_error({event_id, "", name() + " does not support session-based KV reuse"});
    }

    /**
     * Run vision-language (VLM) inference.
     *
     * Default implementation is a no-op — only backends that support VLM
     * override this (currently GenIEBackend).
     *
     * @param images  Preprocessed image tensors (already encoded by the
     *                orchestrator's preprocessVision() adapter). The backend
     *                copies these into IPC shared memory and passes them to
     *                the VLM pipeline — no temp files, no filesystem lifetime
     *                to manage.
     */
    virtual void generateVlm(
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
        std::function<void(const IPCErrorEvent&)>  on_error) {
        // Default: no-op. Backends without VLM support do not override this.
        (void)event_id; (void)session_id; (void)prompt; (void)images; (void)streaming;
        (void)max_tokens; (void)temperature; (void)top_p; (void)top_k;
        (void)presence_penalty; (void)frequency_penalty;
        (void)on_token; (void)on_done; (void)on_error;
    }

    /**
     * Called by SummarizationMiddleware immediately after summarization completes.
     *
     * With the universal post-inference reset (resetKvAsync), this method is
     * now a no-op for GenIEBackend — the KV cache is reset automatically after
     * every generate() / generateVlm() call. Kept for interface compatibility.
     *
     * Default implementation is a no-op.
     */
    virtual void onContextCompacted() {}

    /**
     * Initiate an eager background KV cache reset after inference completes.
     *
     * Called by the orchestrator immediately after generate() / generateVlm()
     * returns. The reset runs in a background thread so it overlaps with
     * returning the response to the HTTP layer and sending it to the client.
     * The next generate() call waits for the reset to complete before sending
     * the EXECUTE command, ensuring clean KV state with minimal added latency.
     *
     * Default implementation is a no-op.
     */
    virtual void resetKvAsync(const std::string& session_id = "") { (void)session_id; }

    /**
     * Wait until the session's post-inference KV reset is complete.
     * The response may already have been delivered to the client; this
     * barrier controls when scheduler capacity and the session reservation
     * can be released.
     */
    virtual void waitForSessionReady(const std::string& session_id = "") {
        (void)session_id;
    }

    /**
     * KV cache checkpoint operations.
     * Only meaningful when capabilities().supports_kv_save_restore == true.
     * Used by reasoning budget management to save/restore KV state around
     * <think> blocks for precise budget enforcement.
     * Default implementations are no-ops.
     */
    virtual void saveKv(const std::string& name, const std::string& session_id = "")    { (void)name; (void)session_id; }
    virtual void restoreKv(const std::string& name, const std::string& session_id = "") { (void)name; (void)session_id; }
    virtual void resetKv(const std::string& session_id = "")                            { (void)session_id; }

    /**
     * Release per-session KV state in the worker.
     * Called when a chat session ends so the worker can free any per-session
     * state. Default is a no-op — only backends with session-scoped KV
     * state override this.
     */
    virtual void clearSession(const std::string& /*session_id*/) {}

    /**
     * Terminate the worker subprocess.
     * @param force  If true, sends SIGKILL immediately (used for client
     *               cancellation via cancelSession()).
     *               If false, sends graceful SHUTDOWN command.
     */
    virtual void terminateWorker(bool force = false) = 0;

    /**
     * Force-kill the active worker subprocess without using model-specific
     * abort APIs. Backends without a killable worker may return false.
     */
    virtual bool forceKillActiveWorker() { return false; }

    /**
     * Cancel a single session's in-flight generation without affecting other
     * concurrent sessions on the same worker.
     *
     * Primary cancellation path for backends where multiple sessions can run
     * concurrently against a shared worker (continuous batching). Callers
     * should fall back to forceKillActiveWorker() if this returns false.
     * Default implementation is a no-op — single-session backends have
     * nothing else running to protect, so forceKillActiveWorker() alone is
     * sufficient for them.
     */
    virtual bool sendAbort(const std::string& session_id) {
        (void)session_id;
        return false;
    }

    /**
     * Returns true if the worker subprocess is alive and responsive.
     */
    virtual bool isHealthy() const = 0;
};

} // namespace qai_forge
