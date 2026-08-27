// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/InternalDTOs.h"
#include "qai_forge/dto/TensorDTOs.h"
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

// ─────────────────────────────────────────────────────────────────────────────
// QaiForge — Unified inference facade
//
// The single public entry point for all inference in qai-forge.
//
// Routing:
//   generate() / generateStream()  → generative models (LLM, VLM)
//                                    via internal scheduler pipeline
//   infer()                        → predictive models (classification,
//                                    detection, segmentation)
//                                    via PredictiveModelPool (Phase 4)
//
// Future modalities (stubs — not yet implemented):
//   transcribe()   → audio-to-text (ASR)
//   synthesize()   → text-to-audio (TTS)
//   embed()        → text/image embeddings
//
// Thread safety: all methods are thread-safe. Multiple callers may call
// generate() concurrently; the internal scheduler serializes per-model.
//
// See docs/qaiforge-api-design.md for the full design rationale.
// ─────────────────────────────────────────────────────────────────────────────

namespace qai_forge {

// ─────────────────────────────────────────────────────────────────────────────
// GenerateOptions — Options for generative AI inference
//
// Passed to generate() and generateStream(). Most fields have sensible
// defaults; callers only need to set what they care about.
//
// Internal scheduler fields (kind, priority, skip_summarization_middleware)
// are set automatically by QaiForge based on which method is called.
// ─────────────────────────────────────────────────────────────────────────────
struct GenerateOptions {
    // Response identity (used by Responses API for tool-chain continuity)
    std::string response_id;
    std::string previous_response_id;
    std::string session_id;

    // Tool-call continuation flags (Responses API only)
    bool tool_output_submission = false;
    bool allow_tool_chain_fallback = false;

    // Response history (Responses API — ancestor messages from ResponseStore)
    bool use_response_history = false;
    json response_history = json::array();

    // Generic conversation memory used to seed orchestrator prompt slots.
    std::string summary_content;
    int summary_token_count = 0;
    std::unordered_map<std::string, std::string> facts;
    std::size_t evicted_message_count = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// StreamCallbacks — Callbacks for async streaming inference
//
// All callbacks are invoked from a background thread (ModelRuntime's executor).
// Callers are responsible for marshaling to their event loop if needed.
// ─────────────────────────────────────────────────────────────────────────────
struct StreamCallbacks {
    // Called for each token/chunk as it arrives (from background thread)
    std::function<void(const StreamChunk&)> onToken;

    // Called when generation completes successfully (from background thread)
    std::function<void(const StandardResponse&)> onComplete;

    // Called if generation fails (from background thread)
    std::function<void(const GenAIException&)> onError;

    // Called if generation is cancelled (from background thread)
    std::function<void()> onCancelled;
};

// ─────────────────────────────────────────────────────────────────────────────
// QaiForge — Unified inference facade
// ─────────────────────────────────────────────────────────────────────────────
class QaiForge {
public:
    static QaiForge& getInstance();

    // ── Generative AI (LLM / VLM) ─────────────────────────────────────────────

    /**
     * Run a blocking generative inference request.
     *
     * Submits the request to the internal scheduler, which manages model
     * residency, queuing, and per-model serialization. Blocks until the
     * response is complete.
     *
     * @param request  Chat completion request (model, messages, params).
     * @param options  Optional: response identity, session, tool-chain flags.
     * @return         Complete StandardResponse with content, tool_calls, usage.
     * @throws GenAIException on model-not-found, queue-full, or inference error.
     */
    StandardResponse generate(
        const CreateChatCompletionRequest& request,
        const GenerateOptions& options = {});

    /**
     * Run an async streaming generative inference request.
     *
     * This method returns immediately after submitting the request to the
     * scheduler. Tokens are delivered via callbacks as they arrive from a
     * background thread. Callers are responsible for marshaling callbacks
     * to their event loop if needed (e.g., using Drogon's queueInLoop).
     *
     * @param request    Chat completion request (stream field set automatically).
     * @param callbacks  Callbacks invoked from background thread as generation proceeds.
     * @param options    Optional: response identity, session, tool-chain flags.
     * @throws GenAIException on model-not-found or queue-full (before async execution).
     *
     * Note: Errors during generation are reported via callbacks.onError, not thrown.
     */
    void generateStream(
        const CreateChatCompletionRequest& request,
        StreamCallbacks callbacks,
        const GenerateOptions& options = {});

    // ── Predictive AI (classification / detection / segmentation) ─────────────

    /**
     * Run a predictive AI tensor inference request.
     *
     * Routes to the correct backend (QNN, SNPE, LiteRT) based on the model's
     * "runtime" field in metadata.json. Stateless — no session, no queuing.
     *
     * @param request  Input tensors (raw bytes) + model ID + output names.
     * @return         Output tensors (raw bytes) + inference stats.
     * @throws GenAIException on model-not-found or inference failure.
     */
    TensorInferenceResponse infer(const TensorInferenceRequest& request);

    // ── Future modalities (not yet implemented) ───────────────────────────────
    //
    // AudioResponse transcribe(const AudioRequest& request);
    //     → ASR: audio bytes → text transcript
    //
    // AudioResponse synthesize(const TextToSpeechRequest& request);
    //     → TTS: text → audio bytes
    //
    // EmbeddingResponse embed(const EmbeddingRequest& request);
    //     → Embeddings: text/image → float vector

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    /**
     * Start the internal scheduler (must be called once at startup).
     * Safe to call multiple times — subsequent calls are no-ops.
     */
    void start();

    /**
     * Shut down the internal scheduler gracefully.
     * @param force  If true, abort in-flight requests immediately.
     */
    void shutdown(bool force = false);

    /**
     * Cancel an in-flight generative request by response_id.
     * @return true if the request was found and cancelled.
     */
    bool cancel(const std::string& response_id);

    QaiForge(const QaiForge&) = delete;
    QaiForge& operator=(const QaiForge&) = delete;

private:
    QaiForge();
    ~QaiForge();

    // Predictive AI pool — owns PredictiveModelRuntime instances.
    // Initialized in start(), shut down in shutdown().
    // Forward-declared to avoid pulling scheduler headers into the public API.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace qai_forge
