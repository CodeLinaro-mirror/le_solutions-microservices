// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/orchestration/OrchestratorAdapter.h"

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

bool memoryChanged(const scheduler::GenieMemoryState& before,
                   const scheduler::GenieMemoryState& after) {
    return before.summary_content != after.summary_content ||
           before.summary_token_count != after.summary_token_count ||
           before.facts != after.facts ||
           before.canonical_boundary != after.canonical_boundary ||
           before.transcript_digest != after.transcript_digest;
}

} // namespace

// PostTurnInput/PostTurnTask — private implementation detail of
// OrchestratorAdapter. The scheduler (ModelRuntime/PostTurnWorker) only
// ever sees the opaque PostTurnWork interface.
namespace {

struct PostTurnInput {
    std::string session_id;
    scheduler::MemoryTurnCommitToken memory_turn;
    scheduler::GenieMemoryState before_memory;
    std::unique_ptr<qai_forge::PreparedGenerationState> prepared_state;
};

// GenerativePostTurnWork — concrete PostTurnWork implementation.
//
// Owns everything OrchestratorAdapter needs to run postTurn() against the
// backend and reconcile the result with ConversationMemoryCoordinator.
// Keeps PostTurnWorker itself fully memory-agnostic.
class GenerativePostTurnWork : public ::PostTurnWork {
public:
    GenerativePostTurnWork(
        PostTurnInput input,
        StandardResponse response,
        std::shared_ptr<qai_forge::IGenerativeOrchestrator> impl,
        std::shared_ptr<scheduler::ConversationMemoryCoordinator> coordinator)
        : ::PostTurnWork(input.session_id),
          input_(std::move(input)),
          response_(std::move(response)),
          impl_(std::move(impl)),
          coordinator_(std::move(coordinator)) {}

    void run(BackendAdapter& backend) noexcept override {
        qai_forge::GenerationJobInfo info;
        info.session_id = input_.session_id;
        info.model_id = response_.model;
        info.response_id = response_.id;
        info.streaming = false;

        if (!input_.prepared_state) {
            LOG_WARN("[OrchestratorAdapter] Post-turn task has no prepared state: session="
                     << input_.session_id);
            coordinator_->publish(input_.memory_turn, scheduler::MemoryOutcome::Failed);
            return;
        }

        std::optional<qai_forge::ConversationMemorySnapshot> updated;
        try {
            updated = impl_->postTurn(
                info, *input_.prepared_state, response_, backend.publicBackend());
        } catch (const std::exception& e) {
            LOG_WARN("[OrchestratorAdapter] postTurn() threw: session="
                     << input_.session_id << " message=\"" << e.what() << "\"");
            coordinator_->publish(input_.memory_turn, scheduler::MemoryOutcome::Failed);
            return;
        } catch (...) {
            LOG_WARN("[OrchestratorAdapter] postTurn() threw: session="
                     << input_.session_id << " message=<unknown>");
            coordinator_->publish(input_.memory_turn, scheduler::MemoryOutcome::Failed);
            return;
        }

        if (!updated.has_value()) {
            coordinator_->publish(input_.memory_turn, scheduler::MemoryOutcome::Unchanged);
            return;
        }

        scheduler::GenieMemoryState after = toInternalMemory(updated.value());
        const scheduler::MemoryOutcome outcome =
            memoryChanged(input_.before_memory, after)
                ? scheduler::MemoryOutcome::Updated
                : scheduler::MemoryOutcome::Unchanged;
        coordinator_->publish(
            input_.memory_turn,
            outcome,
            outcome == scheduler::MemoryOutcome::Updated
                ? std::optional<scheduler::GenieMemoryState>(std::move(after))
                : std::nullopt);
    }

    void cancel() noexcept override {
        coordinator_->abortTurn(
            input_.memory_turn, scheduler::MemoryTurnState::Cancelled);
    }

private:
    PostTurnInput input_;
    StandardResponse response_;
    std::shared_ptr<qai_forge::IGenerativeOrchestrator> impl_;
    std::shared_ptr<scheduler::ConversationMemoryCoordinator> coordinator_;
};

} // namespace

OrchestratorAdapter::OrchestratorAdapter(
    std::shared_ptr<qai_forge::IGenerativeOrchestrator> impl)
    : impl_(std::move(impl)) {}

void OrchestratorAdapter::attachMemoryCoordinator(
    std::shared_ptr<scheduler::ConversationMemoryCoordinator> coordinator) {
    coordinator_ = std::move(coordinator);
}

scheduler::GenerativeJobPtr OrchestratorAdapter::createJob(
    scheduler::GenerativeJobContext context,
    scheduler::GenerativeCallbacks callbacks) const {
    const std::string session_id = context.execution_session_id.empty()
        ? context.job_id
        : context.execution_session_id;

    // Begin the conversation-memory turn (if the caller opted into
    // QaiForge-owned conversation memory for this request) here, rather
    // than in QaiForge::Impl::prepareContext() — this keeps ALL
    // ConversationMemoryCoordinator interaction inside OrchestratorAdapter,
    // out of both the request-admission layer and the scheduler.
    std::optional<scheduler::MemoryTurnCommitToken> memory_turn;
    scheduler::GenieMemoryState initial_memory;
    if (context.caller.conversation.has_value() && coordinator_) {
        scheduler::MemoryTurnStartResult turn_result =
            coordinator_->beginTurn(context.caller.conversation.value());
        if (!turn_result.accepted()) {
            throw GenAIException(
                GenAIErrorCode::INVALID_REQUEST,
                turn_result.message.empty()
                    ? "Conversation turn could not be started"
                    : turn_result.message,
                409);
        }
        memory_turn = turn_result.token;
        initial_memory = turn_result.memory.value_or(scheduler::GenieMemoryState{});
    }

    const qai_forge::ConversationMemorySnapshot memory_snapshot =
        toPublicSnapshot(initial_memory);

    std::unique_ptr<qai_forge::PreparedGenerationState> prepared_state;
    try {
        prepared_state = impl_->prepare(context.request, memory_snapshot, session_id);
    } catch (...) {
        if (memory_turn.has_value()) {
            coordinator_->abortTurn(
                memory_turn.value(), scheduler::MemoryTurnState::GenerationFailed);
        }
        throw;
    }

    auto job = std::make_shared<scheduler::GenerativeJob>();
    job->response_id = context.caller.response_id.empty()
        ? context.job_id
        : std::move(context.caller.response_id);
    job->session_id = session_id;
    job->job_id = std::move(context.job_id);
    job->model_id = std::move(context.model_id);
    job->tool_chain_id = std::move(context.tool_chain_id);
    job->memory_turn = memory_turn;
    job->initial_memory_state = initial_memory;
    job->kind = context.kind;
    job->priority = context.priority;
    job->prepared_state = std::move(prepared_state);
    job->skip_post_turn_summarization =
        context.skip_post_turn_summarization;
    job->callbacks = std::move(callbacks);

    // Wrap on_error/on_cancelled so an aborted/failed job always releases
    // its memory-turn reservation.
    if (memory_turn.has_value()) {
        std::shared_ptr<scheduler::ConversationMemoryCoordinator> coordinator =
            coordinator_;
        scheduler::MemoryTurnCommitToken token = memory_turn.value();

        auto original_on_error = job->callbacks.on_error;
        job->callbacks.on_error =
            [coordinator, token, original_on_error](const GenAIException& error) {
                coordinator->abortTurn(
                    token, scheduler::MemoryTurnState::GenerationFailed);
                if (original_on_error) {
                    original_on_error(error);
                }
            };

        auto original_on_cancelled = job->callbacks.on_cancelled;
        job->callbacks.on_cancelled =
            [coordinator, token, original_on_cancelled]() {
                coordinator->abortTurn(
                    token, scheduler::MemoryTurnState::Cancelled);
                if (original_on_cancelled) {
                    original_on_cancelled();
                }
            };
    }

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

std::unique_ptr<PostTurnWork> OrchestratorAdapter::createPostTurnTask(
    scheduler::GenerativeJob& job,
    const StandardResponse& response) const {
    if (!job.memory_turn.has_value() ||
        job.skip_post_turn_summarization ||
        !job.prepared_state) {
        if (job.memory_turn.has_value() && coordinator_) {
            const bool has_tool_calls = response.finish_reason == "tool_calls" &&
                response.tool_calls.has_value() &&
                !response.tool_calls.value().empty();
            if (has_tool_calls && job.kind == scheduler::JobKind::MCP_ROUND) {
                coordinator_->markAwaitingTool(job.memory_turn.value());
            } else {
                coordinator_->completeStructural(job.memory_turn.value());
            }
        }
        return nullptr;
    }

    const bool began_post_turn =
        coordinator_->beginPostTurn(job.memory_turn.value());
    if (!began_post_turn) {
        LOG_WARN("[OrchestratorAdapter] Post-turn state transition rejected: session="
                 << job.session_id);
        coordinator_->abortTurn(
            job.memory_turn.value(), scheduler::MemoryTurnState::GenerationFailed);
        return nullptr;
    }

    PostTurnInput input;
    input.session_id = job.session_id;
    input.memory_turn = job.memory_turn.value();
    input.before_memory = job.initial_memory_state;
    input.prepared_state = std::move(job.prepared_state);

    return std::make_unique<GenerativePostTurnWork>(
        std::move(input), response, impl_, coordinator_);
}
