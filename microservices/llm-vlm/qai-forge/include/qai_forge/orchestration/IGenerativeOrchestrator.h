// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/InternalDTOs.h"
#include "qai_forge/backend/IGenerativeBackend.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

// ─────────────────────────────────────────────────────────────────────────────
// qai_forge::IGenerativeOrchestrator — public, out-of-tree ("Level B")
// orchestrator ABI.
//
// This is the contract a third party implements to plug a new generative
// model-family orchestrator into qai-forge without access to qai-forge's
// source tree or internal scheduler plumbing.
//
// NOTE on naming: qai-forge also has an internal, scheduler-facing
// ::IGenerativeOrchestrator (global namespace, declared in
// internal/qai_forge/orchestration/IGenerativeOrchestrator.h) — that type
// carries MemoryTurnCommitToken/ConversationMemoryCoordinator capability
// tokens and a closed std::variant of prepared-request types that cannot be
// safely or practically exposed to a third-party .so. This public interface
// (qai_forge::IGenerativeOrchestrator, namespaced) is intentionally a
// different type with the same preferred name — disambiguated by namespace,
// not spelling. internal/qai_forge/orchestration/OrchestratorAdapter.{h,cpp}
// wraps any registered qai_forge::IGenerativeOrchestrator and implements the
// private ::IGenerativeOrchestrator on top of it, translating opaque
// PreparedGenerationState/ConversationMemorySnapshot values to/from the
// private scheduler types. A third-party implementation of this public
// interface never sees any of that.
//
// Backend registration (registerGenerative/registerPredictive on
// PluginRegistry) remains internal-only for now — this interface only
// covers the orchestrator half of a generative runtime plugin. Promoting
// backend registration itself to a public ABI is a follow-up phase.
// ─────────────────────────────────────────────────────────────────────────────

namespace qai_forge {

// ─────────────────────────────────────────────────────────────────────────────
// ConversationMemorySnapshot — public, renamed replacement for the internal
// GenieMemoryState DTO. Fields are 100% model-agnostic (rolling summary,
// extracted facts, eviction boundary, transcript integrity digest) — this is
// the full state an orchestrator may persist privately across turns of a
// QaiForge-owned conversation lineage (see qai_forge::ConversationReference).
//
// An orchestrator with no private per-turn memory (e.g. a stateless backend
// that always resends full context) simply never populates
// canonical_boundary/summary_content/facts and returns std::nullopt from
// postTurn() — see IGenerativeOrchestrator::postTurn()'s default.
// ─────────────────────────────────────────────────────────────────────────────
struct ConversationMemorySnapshot {
    std::string summary_content;
    int summary_token_count = 0;
    std::unordered_map<std::string, std::string> facts;
    std::size_t canonical_boundary = 0;
    std::string transcript_digest;
};

// ─────────────────────────────────────────────────────────────────────────────
// PreparedGenerationState — opaque polymorphic base.
//
// Replaces the old closed std::variant<GeniePreparedRequest,
// LlamaCppPreparedRequest, LiteRTLMPreparedRequest> that lived directly on
// the internal GenerativeJob — that variant could never grow a 4th backend
// type without editing qai-forge's own header.
//
// Each orchestrator implementation defines its own private concrete
// subclass (e.g. GenieOrchestrator declares a private nested PreparedState
// struct) holding whatever fields it needs between prepare() and execute()/
// postTurn() — the final rendered prompt, generation sampling parameters,
// vision buffers, tool list, anything. Scheduler/runtime code (and the
// OrchestratorAdapter) only ever holds a unique_ptr<PreparedGenerationState>
// and passes it back to the same orchestrator instance opaquely; it never
// inspects the concrete type.
// ─────────────────────────────────────────────────────────────────────────────
struct PreparedGenerationState {
    virtual ~PreparedGenerationState() = default;
};

// ─────────────────────────────────────────────────────────────────────────────
// GenerationJobInfo — identity/context fields an orchestrator needs during
// execute()/postTurn(), without exposing the internal scheduler's
// GenerativeJob type (which carries MemoryTurnCommitToken, JobKind/
// JobPriority, cancellation atomics, etc.).
// ─────────────────────────────────────────────────────────────────────────────
struct GenerationJobInfo {
    std::string job_id;
    std::string model_id;
    std::string session_id;
    std::string response_id;
    bool streaming = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// IGenerativeOrchestrator — implement this to add a new generative
// model-family orchestrator.
// ─────────────────────────────────────────────────────────────────────────────
class IGenerativeOrchestrator {
public:
    virtual ~IGenerativeOrchestrator() = default;

    /**
     * Build the backend-specific prepared request from the raw caller
     * request and the rehydrated conversation memory (empty/default if the
     * caller did not opt into QaiForge-owned conversation memory for this
     * turn — see qai_forge::ConversationReference).
     *
     * This is where prompt assembly, reasoning-budget calculation, context
     * eviction, inline-image extraction, etc. happen — anything the
     * orchestrator needs to decide once per turn, before execute() runs.
     *
     * @param request    The raw chat completion request.
     * @param memory     Rehydrated private conversation memory for this
     *                   session (default-constructed/empty if the caller
     *                   did not opt into private memory for this request).
     * @param session_id Stable session identifier for this execution.
     * @return           Opaque prepared state passed back into execute()
     *                   and (optionally) postTurn() for the same job.
     */
    virtual std::unique_ptr<PreparedGenerationState> prepare(
        const CreateChatCompletionRequest& request,
        const ConversationMemorySnapshot& memory,
        const std::string& session_id) const = 0;

    /**
     * Run inference against the backend using the prepared state from
     * prepare(). on_token is non-null iff this is a streaming request —
     * call it zero or more times with partial StreamChunk deltas before
     * returning the final StandardResponse.
     */
    virtual StandardResponse execute(
        const GenerationJobInfo& info,
        PreparedGenerationState& prepared,
        qai_forge::IGenerativeBackend& backend,
        std::function<void(const StreamChunk&)> on_token) const = 0;

    /**
     * Optional post-turn bookkeeping — summarization, fact extraction,
     * context eviction, or any other work the orchestrator wants to run
     * AFTER execute() has returned and the response has already been
     * delivered to the caller. Called on a dedicated worker thread, never
     * blocking the response path.
     *
     * Return the updated ConversationMemorySnapshot to persist it for the
     * next turn in this conversation lineage, or std::nullopt if nothing
     * changed (or this orchestrator has no private per-turn memory at all —
     * the default implementation below covers that case; most backends
     * never need to override this).
     */
    virtual std::optional<ConversationMemorySnapshot> postTurn(
        const GenerationJobInfo& info,
        PreparedGenerationState& prepared,
        const StandardResponse& response,
        qai_forge::IGenerativeBackend& backend) const {
        (void)info;
        (void)prepared;
        (void)response;
        (void)backend;
        return std::nullopt;
    }
};

} // namespace qai_forge
