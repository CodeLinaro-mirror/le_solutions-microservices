// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/backend/BackendAdapter.h"
#include "qai_forge/orchestration/IGenerativeOrchestrator.h"
#include "qai_forge/scheduler/GenerativeJob.h"

#include <memory>
#include <optional>

// ─────────────────────────────────────────────────────────────────────────────
// OrchestratorAdapter — internal glue between the public
// qai_forge::IGenerativeOrchestrator ABI and the scheduler.
//
// This is a concrete class, not an interface — every runtime orchestrator
// (Genie/LiteRTLM/LlamaCpp, and any future third-party plugin) implements
// the public qai_forge::IGenerativeOrchestrator contract, and
// PluginRegistry::createGenerativeOrchestrator() wraps the registered
// instance in exactly one of these before handing it to
// ModelRuntime/WarmModelPool/PostTurnWorker. There is intentionally no
// separate scheduler-facing interface above this class — with every
// concrete orchestrator now implementing the public ABI, an abstract base
// with a single implementer added nothing but an extra vtable indirection
// and (previously) a confusing same-name type collision with the public
// interface. ModelRuntime etc. simply hold/pass
// std::shared_ptr<OrchestratorAdapter> directly.
//
// Responsibilities owned entirely here (never exposed to the wrapped public
// orchestrator):
//   - Translating GenerativeJobContext/GenerativeJob (which carry
//     MemoryTurnCommitToken, JobKind/JobPriority, cancellation atomics) to/
//     from the public GenerationJobInfo + ConversationMemorySnapshot types.
//   - Managing the ConversationMemoryCoordinator commit protocol
//     (beginPostTurn/abortTurn/publish) around postTurn() calls.
//   - Storing the opaque qai_forge::PreparedGenerationState produced by
//     prepare() on GenerativeJob::prepared_state / PostTurnTask::input.
// ─────────────────────────────────────────────────────────────────────────────
class OrchestratorAdapter {
public:
    explicit OrchestratorAdapter(
        std::shared_ptr<qai_forge::IGenerativeOrchestrator> impl);

    scheduler::GenerativeJobPtr createJob(
        scheduler::GenerativeJobContext context,
        scheduler::GenerativeCallbacks callbacks) const;

    StandardResponse execute(
        scheduler::GenerativeJob& job,
        BackendAdapter& backend) const;

    std::optional<scheduler::PostTurnTask> createPostTurnTask(
        scheduler::GenerativeJob& job,
        const StandardResponse& response) const;

    scheduler::GenieMemoryState executePostTurn(
        scheduler::PostTurnTask& task,
        BackendAdapter& backend) const;

private:
    std::shared_ptr<qai_forge::IGenerativeOrchestrator> impl_;
};
