// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/orchestration/IOrchestrator.h"
#include "qai_forge/backend/LiteRTLMBackend.h"
#include "qai_forge/session/SessionManager.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <mutex>

// ─────────────────────────────────────────────────────────────────────────────
// LiteRTLMOrchestrator — Phase 4: IOrchestrator for LiteRT-LM
//
// Implements the IOrchestrator interface for LiteRT-LM models. Mirrors the
// GenieOrchestrator design: all intelligence lives in the orchestrator (main
// process), while the worker subprocess handles only raw inference.
//
// LiteRT-LM-specific responsibilities:
//   1. Jinja prompt rendering — uses the jinja_template from model metadata
//      to render chat messages into a flat prompt string. The template is
//      extracted from the .litertlm model file by the worker and sent to the
//      orchestrator via the METADATA IPC message.
//   2. Tool call parsing — detects tool_call_delimiter in generated text and
//      parses tool calls using a simple JSON extractor.
//   3. Context overflow handling — implements sliding window eviction to keep
//      the prompt within max_context_length. Falls back to summarization if
//      the sliding window alone is insufficient.
//   4. Universal post-inference KV reset — calls backend.resetKvAsync() after
//      every generate() call, same as GenieOrchestrator.
//
// Lazy metadata initialization:
//   The orchestrator is constructed with no backend reference and no metadata.
//   On the first execute() call, it dynamic_cast's the IGenerativeBackend& to
//   LiteRTLMBackend* and reads the metadata (jinja_template, max_context_length,
//   tool_call_delimiter) that the worker sent via the METADATA IPC event during
//   model load. This avoids the double-load bug that would occur if the
//   orchestrator tried to load the model at construction time.
//
// Each ModelRuntime owns one LiteRTLMOrchestrator instance (created by
// BackendFactory::createRuntimePair). The orchestrator is constructed before
// the model is loaded — metadata is populated lazily on the first execute().
// ─────────────────────────────────────────────────────────────────────────────

class LiteRTLMOrchestrator : public IOrchestrator {
public:
    using CancellationPredicate = std::function<bool()>;

    /**
     * Default constructor — creates an orchestrator with no metadata.
     * Metadata is initialized lazily on the first execute() call, after
     * ModelRuntime has called backend_->loadModel() and the worker has
     * sent the METADATA IPC event.
     */
    LiteRTLMOrchestrator() = default;

    // ── IOrchestrator interface ────────────────────────────────────────────────

    /**
     * Execute one inference turn with an injected backend.
     *
     * On the first call, initializes metadata from the backend (lazy init).
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
     * Execute a blocking chat request using the supplied backend.
     */
    StandardResponse executeBlocking(
        const CreateChatCompletionRequest& request,
        IGenerativeBackend& backend,
        CancellationPredicate cancel_requested = {});

    /**
     * Execute a streaming chat request using the supplied backend.
     */
    StandardResponse executeStreaming(
        const CreateChatCompletionRequest& request,
        IGenerativeBackend& backend,
        OrchestratorStreamCallback callback,
        CancellationPredicate cancel_requested = {});

private:
    LiteRTLMOrchestrator(const LiteRTLMOrchestrator&) = delete;
    LiteRTLMOrchestrator& operator=(const LiteRTLMOrchestrator&) = delete;

    // ── Lazy metadata initialization ───────────────────────────────────────────

    /**
     * Initialize metadata from the backend on the first execute() call.
     * Thread-safe via metadata_mutex_. No-op if already initialized.
     *
     * @param backend  The IGenerativeBackend passed to execute(). Must be a
     *                 LiteRTLMBackend with a loaded model (worker running and
     *                 METADATA IPC event already received).
     * @return true if metadata was successfully initialized, false otherwise.
     */
    bool initMetadataIfNeeded(IGenerativeBackend& backend);

    // ── Prompt rendering ───────────────────────────────────────────────────────

    /**
     * Render chat messages to a flat prompt string using the Jinja template.
     *
     * Falls back to a simple role: content format if the template is empty
     * or rendering fails.
     *
     * @param messages  JSON array of {role, content} message objects.
     * @param tools     Optional JSON array of tool definitions.
     * @param add_generation_prompt  Whether to append the generation prompt suffix.
     * @return Rendered prompt string ready for the worker.
     */
    std::string renderPrompt(const json& messages,
                             const json& tools,
                             bool add_generation_prompt = true) const;

    /**
     * Lightweight Jinja2 template renderer for chat templates.
     * Handles the common subset: for loops, if/else, variable substitution,
     * whitespace control (-), and string filters (upper, lower, strip).
     */
    std::string renderJinja(const std::string& tmpl,
                            const json& context) const;

    // ── Context management ─────────────────────────────────────────────────────

    /**
     * Estimate the token count of a string.
     * Uses a simple heuristic: ~4 characters per token (BPE approximation).
     */
    int estimateTokens(const std::string& text) const;

    /**
     * Apply sliding window eviction to keep the prompt within context limits.
     *
     * @param messages  Message array to evict from (modified in place).
     * @param tools     Tool definitions (included in token estimate).
     * @return Number of messages evicted.
     */
    int applyContextEviction(json& messages, const json& tools) const;

    // ── Tool call parsing ──────────────────────────────────────────────────────

    /**
     * Check if the generated text contains a tool call delimiter.
     */
    bool hasToolCallDelimiter(const std::string& text) const;

    /**
     * Parse tool calls from generated text.
     *
     * @param text  Generated text containing tool call delimiter + JSON.
     * @return JSON array of tool call objects, or null if parsing fails.
     */
    json parseToolCalls(const std::string& text) const;

    /**
     * Extract the text before the tool call delimiter.
     */
    std::string extractPreToolContent(const std::string& text) const;

    // ── Core execution ─────────────────────────────────────────────────────────

    StandardResponse executeCore(
        const CreateChatCompletionRequest& request,
        IGenerativeBackend& backend,
        OrchestratorStreamCallback callback,
        const CancellationPredicate& cancel_requested);

    // ── State ──────────────────────────────────────────────────────────────────

    // Metadata from the LiteRT-LM model — populated lazily on first execute()
    std::string jinja_template_;
    std::string model_type_;
    int         max_context_length_      = 4096;
    std::string tool_call_delimiter_;
    std::string tool_response_delimiter_;

    // Lazy initialization state
    bool        metadata_initialized_    = false;
    mutable std::mutex metadata_mutex_;

    // Compaction threshold: evict when prompt exceeds this fraction of context
    static constexpr float kCompactionThreshold = 0.80f;
};
