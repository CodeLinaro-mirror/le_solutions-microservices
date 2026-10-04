// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/backend/BackendAdapter.h"
#include "qai_forge/orchestration/IGenerativeOrchestrator.h"
#include "qai_forge/scheduler/ConversationMemoryCoordinator.h"
#include "qai_forge/scheduler/GenerativeJob.h"

#include <memory>
#include <optional>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// PostTurnWork — opaque deferred-work handle.
//
// ModelRuntime/PostTurnWorker (the scheduler's job executors) only ever see
// this interface — they have zero knowledge of conversation memory,
// MemoryTurnCommitToken, or ConversationMemoryCoordinator. All of that
// bookkeeping lives inside the concrete subclass that OrchestratorAdapter
// constructs internally (see OrchestratorAdapter.cpp). This keeps the
// scheduler's job-sequencing/capacity-admission responsibilities cleanly
// separated from orchestrator-owned conversation-memory concerns.
// ─────────────────────────────────────────────────────────────────────────────
class PostTurnWork {
public:
    virtual ~PostTurnWork() = default;

    const std::string& sessionId() const { return session_id_; }

    // Runs the deferred work against the backend. Never throws — any
    // orchestrator-specific failure handling (e.g. publishing a Failed
    // outcome to the memory coordinator) happens internally.
    virtual void run(BackendAdapter& backend) noexcept = 0;

    // Called instead of run() when the runtime is force-stopping and the
    // work was never dispatched. Default no-op for implementations with
    // nothing extra to clean up.
    virtual void cancel() noexcept {}

protected:
    explicit PostTurnWork(std::string session_id)
        : session_id_(std::move(session_id)) {}

private:
    std::string session_id_;
};

// ─────────────────────────────────────────────────────────────────────────────
// OrchestratorAdapter — internal glue between the public
// qai_forge::IGenerativeOrchestrator ABI and the scheduler.
//
// This is a concrete class, not an interface — every runtime orchestrator
// (Genie/LiteRTLM/LlamaCpp, and any future third-party plugin) implements
// the public qai_forge::IGenerativeOrchestrator contract, and
// PluginRegistry::createGenerativeOrchestrator() wraps the registered
// instance in exactly one of these before handing it to
// ModelRuntime/WarmModelPool/PostTurnWorker. ModelRuntime etc. simply
// hold/pass std::shared_ptr<OrchestratorAdapter> directly.
//
// Responsibilities owned entirely here (never exposed to the wrapped public
// orchestrator, and never exposed to ModelRuntime/PostTurnWorker either):
//   - Translating GenerativeJobContext/GenerativeJob (which carry
//     MemoryTurnCommitToken, JobKind/JobPriority, cancellation atomics) to/
//     from the public GenerationJobInfo + ConversationMemorySnapshot types.
//   - Owning ALL interaction with ConversationMemoryCoordinator — beginning
//     turns in createJob(), aborting them if a job fails/cancels, and
//     running/publishing post-turn work via the opaque PostTurnWork
//     returned from createPostTurnTask(). The scheduler (ModelRuntime/
//     PostTurnWorker) never touches MemoryTurnCommitToken/MemoryOutcome/
//     MemoryTurnState/GenieMemoryState directly — it only knows whether an
//     opaque PostTurnWork exists for a given job, for capacity/eviction
//     admission purposes.
//   - Storing the opaque qai_forge::PreparedGenerationState produced by
//     prepare() on GenerativeJob::prepared_state / the internal post-turn
//     work object.
// ─────────────────────────────────────────────────────────────────────────────
class OrchestratorAdapter {
public:
    explicit OrchestratorAdapter(
        std::shared_ptr<qai_forge::IGenerativeOrchestrator> impl);

    // Injected post-construction by WarmModelPool::getOrCreateRuntimeLocked()
    // — PluginRegistry::createGenerativeOrchestrator() has no access to the
    // scheduler's ConversationMemoryCoordinator (a different subsystem
    // entirely), so wiring happens at the one place both are already
    // available: WarmModelPool, right after the RuntimePair is created.
    void attachMemoryCoordinator(
        std::shared_ptr<scheduler::ConversationMemoryCoordinator> coordinator);

    scheduler::GenerativeJobPtr createJob(
        scheduler::GenerativeJobContext context,
        scheduler::GenerativeCallbacks callbacks) const;

    StandardResponse execute(
        scheduler::GenerativeJob& job,
        BackendAdapter& backend) const;

    // Returns nullptr if there is no deferred post-turn work for this job
    // (e.g. no conversation memory in use, a tool call is pending, or the
    // caller opted out of post-turn summarization). Otherwise returns an
    // opaque PostTurnWork ready to be enqueued on PostTurnWorker — the
    // scheduler never inspects its concrete type.
    std::unique_ptr<PostTurnWork> createPostTurnTask(
        scheduler::GenerativeJob& job,
        const StandardResponse& response) const;

private:
    std::shared_ptr<qai_forge::IGenerativeOrchestrator> impl_;
    std::shared_ptr<scheduler::ConversationMemoryCoordinator> coordinator_;
};
