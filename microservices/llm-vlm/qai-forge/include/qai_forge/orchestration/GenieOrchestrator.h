// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/orchestration/IOrchestrator.h"
#include "qai_forge/session/SessionManager.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/backend/IGenerativeBackend.h"
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// GenieOrchestrator — Genie-specific IOrchestrator Implementation
//
// Implements the IOrchestrator interface for the Qualcomm GenIE SDK backend.
// This is the concrete orchestrator for all Genie-based LLM and VLM models.
//
// Genie-specific responsibilities:
//   1. Universal post-inference KV reset via backend.resetKvAsync() — called
//      immediately after every generate() / generateVlm() so the reset runs
//      concurrently with returning the response to the HTTP layer.
//   2. Slot-based prompt assembly — system / summary / history slots with
//      proportional token ceilings scaled to the model's context window.
//   3. Genie-specific reasoning budget calculation via ReasoningBudgetCalculator.
//
// Each ModelRuntime owns one GenieOrchestrator instance (created by
// BackendFactory::createRuntimePair). The orchestrator is stateless with
// respect to model backends — it receives the backend as an injected parameter
// on each execute() call.
//
// The request flows through a clean pipeline:
//   Step 1: Extractor     — parse messages and config from the request DTO
//   Step 2: Session Resolver — find/create ConversationSession, issue DraftTurn
//   Step 3: Context prep   — prompt construction
//   Step 4: Execution      — injected backend → ReasoningRouter → yield DTOs
//   Step 5: Post-turn      — resetKvAsync
// ─────────────────────────────────────────────────────────────────────────────
class GenieOrchestrator : public IOrchestrator {
public:
    using CancellationPredicate = std::function<bool()>;

    // Constructor — public so BackendFactory::createRuntimePair() can create
    // per-model instances for the scheduler path.
    GenieOrchestrator();

    static GenieOrchestrator& getInstance();

    // ── IOrchestrator interface ────────────────────────────────────────────────
    /**
     * Execute one inference turn with an injected backend.
     *
     * This is the primary entry point for the scheduler path (ModelRuntime).
     * Creates a transient ConversationSession seeded from response_history.
     * Blocking when callback is nullptr; streaming otherwise.
     */
    StandardResponse execute(
        const CreateChatCompletionRequest& request,
        const json& response_history,
        IGenerativeBackend& backend,
        OrchestratorStreamCallback callback,
        std::function<bool()> cancel) override;

    /**
     * @brief Execute a blocking chat request using the supplied backend.
     * @detail Scheduler-safe entry point: no global concurrency guard is taken
     *         and no singleton backend is used.
     */
    StandardResponse executeBlocking(
        const CreateChatCompletionRequest& request,
        IGenerativeBackend& backend,
        CancellationPredicate cancel_requested = {},
        bool skip_summarization_middleware = false);

    /**
     * @brief Execute a streaming chat request using the supplied backend.
     * @detail Scheduler-safe entry point: no global concurrency guard is taken
     *         and no singleton backend is used.
     */
    StandardResponse executeStreaming(
        const CreateChatCompletionRequest& request,
        IGenerativeBackend& backend,
        OrchestratorStreamCallback callback,
        CancellationPredicate cancel_requested = {},
        bool skip_summarization_middleware = false);

private:
    GenieOrchestrator(const GenieOrchestrator&) = delete;
    GenieOrchestrator& operator=(const GenieOrchestrator&) = delete;

    // ── Pipeline steps ─────────────────────────────────────────────────────────

    /**
     * Step 1: Extract and validate the request.
     * Throws GenAIException(INVALID_REQUEST) if model is not found.
     */
    void validateRequest(const CreateChatCompletionRequest& request);

    /**
     * Step 2: Resolve or create a ConversationSession and issue a DraftTurn.
     * The session_id is derived from the request (user field or hash-based lookup).
     */
    std::pair<std::shared_ptr<ConversationSession>, DraftTurn>
    resolveSessionAndDraft(const CreateChatCompletionRequest& request);

    /**
     * Build the compacted context prompt for the inference worker.
     *
     * Slot assembly (highest priority = never evicted):
     *   Slot 1 — System block:      caller's system prompt + tool instructions
     *   Slot 1.5 — Budget info:     reasoning budget notification (if applicable)
     *   Slot 2 — Persistent facts:  key-value facts extracted from evicted history
     *   Slot 3 — Episodic summary:  rolling summary of evicted history
     *   Slot 4 — History queue:     recent turns not yet evicted, oldest-first
     *   Slot 5 — Current turn:      new user/tool messages
     */
    std::string buildContextPrompt(const ConversationSession& session,
                                   const CreateChatCompletionRequest& request,
                                   int thinking_budget = 0,
                                   int answer_budget = 0) const;

    StandardResponse executeBlockingPrepared(
        const CreateChatCompletionRequest& request,
        std::shared_ptr<ConversationSession> session,
        DraftTurn&& draft,
        IGenerativeBackend& backend,
        const CancellationPredicate& cancel_requested,
        bool skip_summarization_middleware,
        bool register_session_hash);

    StandardResponse executeStreamingPrepared(
        const CreateChatCompletionRequest& request,
        std::shared_ptr<ConversationSession> session,
        DraftTurn&& draft,
        IGenerativeBackend& backend,
        OrchestratorStreamCallback callback,
        const CancellationPredicate& cancel_requested,
        bool skip_summarization_middleware,
        bool register_session_hash);

    // ── Post-turn memory management ────────────────────────────────────────────

    /**
     * Run a summarization inference pass on the given messages.
     *
     * Builds a structured summarization prompt, calls backend.generate() with
     * low temperature, and returns (summary_text, token_count).
     * The KV cache must be clean before this call (caller ensures this via
     * backend.resetKvAsync() + waitForPendingReset() inside executeRequest).
     *
     * @param session               Current session (for prior summary context).
     * @param messages_to_summarize Messages to summarize (eviction batch).
     * @param model_id              Model identifier for chat template lookup.
     * @param max_summary_tokens    Token budget for the generated summary.
     * @param backend               Backend to run inference on.
     * @return (summary_text, token_count) — empty string on failure.
     */
    std::pair<std::string, int> generateSummary(
        const ConversationSession& session,
        const std::vector<json>& messages_to_summarize,
        const std::string& model_id,
        int max_summary_tokens,
        IGenerativeBackend& backend) const;

    /**
     * Run a fact extraction inference pass on the given messages.
     *
     * Asks the model to return a JSON object of key-value facts. Parses the
     * response and merges new facts into session.facts.
     *
     * @param session               Session to update (facts written here).
     * @param messages_to_extract   Messages to extract facts from.
     * @param model_id              Model identifier for chat template lookup.
     * @param backend               Backend to run inference on.
     */
    void extractFacts(ConversationSession& session,
                      const std::vector<json>& messages_to_extract,
                      const std::string& model_id,
                      IGenerativeBackend& backend) const;

    /**
     * Post-turn memory management: eviction, summarization, and fact extraction.
     *
     * Called after every complete non-tool-call LLM turn (not VLM, not tool
     * continuations). Runs synchronously before returning the response so the
     * next turn's prompt is fully prepared.
     *
     * Sequence:
     *   1. Estimate history token usage (messages[evicted_message_count..end])
     *   2. If over eviction threshold: identify oldest messages to evict
     *   3. Summarize eviction batch → session.summary_content updated
     *   4. backend.resetKvAsync() — reset KV after summarization inference
     *   5. Extract facts from eviction batch → session.facts updated
     *   6. backend.resetKvAsync() — reset KV after fact extraction inference
     *   7. session.evicted_message_count += eviction batch size
     *
     * Thresholds (matching Python service):
     *   eviction_threshold = 0.80 × history_budget
     *   eviction_target    = 0.60 × history_budget
     *   history_budget     = context_size × 0.40
     *
     * @param session    Session to update.
     * @param model_id   Model identifier (for context size + chat template).
     * @param backend    Backend to run summarization/fact inference on.
     */
    void postTurnProcessing(ConversationSession& session,
                            const std::string& model_id,
                            IGenerativeBackend& backend) const;
};
