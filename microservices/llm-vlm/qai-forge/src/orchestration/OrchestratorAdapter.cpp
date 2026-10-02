// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/orchestration/OrchestratorAdapter.h"

#include "qai_forge/scheduler/ConversationMemoryCoordinator.h"
#include "qai_forge/utils/Logger.h"

#include <utility>

namespace {

qai_forge::ConversationMemorySnapshot toPublicSnapshot(
    const scheduler::GenieMemoryState& memory) {
    qai_forge::ConversationMemorySnapshot snapshot;
    snapshot.summary_content = memory.summary_content;
    snapshot.summary_token_count = memory.summary_token_count;
    snapshot.facts = memory.facts;
    snapshot.canonical_boundary = memory.canonical_boundary;
    snapshot.transcript_digest = memory.transcript_digest;
    return snapshot;
}

scheduler::GenieMemoryState toInternalMemory(
    const qai_forge::ConversationMemorySnapshot& snapshot) {
    scheduler::GenieMemoryState memory;
    memory.summary_content = snapshot.summary_content;
    memory.summary_token_count = snapshot.summary_token_count;
    memory.facts = snapshot.facts;
    memory.canonical_boundary = snapshot.canonical_boundary;
    memory.transcript_digest = snapshot.transcript_digest;
    return memory;
}

qai_forge::GenerationJobInfo toJobInfo(const scheduler::GenerativeJob& job,
                                       bool streaming) {
    qai_forge::GenerationJobInfo info;
    info.job_id = job.job_id;
    info.model_id = job.model_id;
    info.session_id = job.session_id;
    info.response_id = job.response_id;
    info.streaming = streaming;
    return info;
}

} // namespace

OrchestratorAdapter::OrchestratorAdapter(
    std::shared_ptr<qai_forge::IGenerativeOrchestrator> impl)
    : impl_(std::move(impl)) {}

scheduler::GenerativeJobPtr OrchestratorAdapter::createJob(
    scheduler::GenerativeJobContext context,
    scheduler::GenerativeCallbacks callbacks) const {
    const std::string session_id = context.execution_session_id.empty()
        ? context.job_id
        : context.execution_session_id;

    const scheduler::GenieMemoryState initial_memory =
        context.memory_state.value_or(scheduler::GenieMemoryState{});
    const qai_forge::ConversationMemorySnapshot memory_snapshot =
        toPublicSnapshot(initial_memory);

    auto prepared_state =
        impl_->prepare(context.request, memory_snapshot, session_id);

    auto job = std::make_shared<scheduler::GenerativeJob>();
    job->response_id = context.caller.response_id.empty()
        ? context.job_id
        : std::move(context.caller.response_id);
    job->session_id = session_id;
    job->job_id = std::move(context.job_id);
    job->model_id = std::move(context.model_id);
    job->tool_chain_id = std::move(context.tool_chain_id);
    job->memory_turn = context.memory_turn;
    job->initial_memory_state = initial_memory;
    job->kind = context.kind;
    job->priority = context.priority;
    job->prepared_state = std::move(prepared_state);
    job->skip_post_turn_summarization =
        context.skip_post_turn_summarization;
    job->callbacks = std::move(callbacks);
    return job;
}

StandardResponse OrchestratorAdapter::execute(
    scheduler::GenerativeJob& job,
    BackendAdapter& backend) const {
    if (!job.prepared_state) {
        throw std::runtime_error(
            "OrchestratorAdapter received a job with no prepared state");
    }

    const bool streaming = static_cast<bool>(job.callbacks.on_token);
    const qai_forge::GenerationJobInfo info = toJobInfo(job, streaming);

    // Forward streaming callbacks directly — IGenerativeOrchestrator::
    // execute() takes a plain std::function, no internal types involved.
    std::function<void(const StreamChunk&)> on_token;
    if (streaming) {
        on_token = [&job](const StreamChunk& chunk) {
            if (job.callbacks.on_token) {
                job.callbacks.on_token(chunk);
            }
        };
    }

    return impl_->execute(
        info, *job.prepared_state, backend.publicBackend(), on_token);
}

std::optional<scheduler::PostTurnTask> OrchestratorAdapter::createPostTurnTask(
    scheduler::GenerativeJob& job,
    const StandardResponse& response) const {
    if (!job.memory_turn.has_value() ||
        job.skip_post_turn_summarization ||
        !job.prepared_state) {
        return std::nullopt;
    }
    (void)response;

    scheduler::PostTurnTask task;
    task.input.session_id = job.session_id;
    task.input.memory_turn = job.memory_turn.value();
    task.input.before_memory = job.initial_memory_state;
    task.input.prepared_state = std::move(job.prepared_state);
    task.response = response;
    return task;
}

scheduler::GenieMemoryState OrchestratorAdapter::executePostTurn(
    scheduler::PostTurnTask& task,
    BackendAdapter& backend) const {
    qai_forge::GenerationJobInfo info;
    info.session_id = task.input.session_id;
    info.model_id = task.response.model;
    info.response_id = task.response.id;
    info.streaming = false;

    if (!task.input.prepared_state) {
        LOG_WARN("[OrchestratorAdapter] Post-turn task has no prepared state: session="
                 << task.input.session_id);
        return task.input.before_memory;
    }

    std::optional<qai_forge::ConversationMemorySnapshot> updated;
    try {
        updated = impl_->postTurn(
            info, *task.input.prepared_state, task.response,
            backend.publicBackend());
    } catch (const std::exception& e) {
        LOG_WARN("[OrchestratorAdapter] postTurn() threw: session="
                 << task.input.session_id << " message=\"" << e.what() << "\"");
        return task.input.before_memory;
    }

    if (!updated.has_value()) {
        return task.input.before_memory;
    }
    return toInternalMemory(updated.value());
}
