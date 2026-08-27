// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// QaiForge.cpp — Unified inference facade implementation
//
// Thin routing layer over the internal scheduler (generative) and
// PredictiveModelPool (predictive). All scheduling complexity stays
// in the internal scheduler namespace.
//
// Phase 4 change: infer() now routes through PredictiveModelPool instead of
// the deprecated PredictiveAIOrchestrator singleton. The pool manages model
// residency, LRU eviction, and concurrent inference across different models.
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/QaiForge.h"
#include "qai_forge/scheduler/ModelScheduler.h"
#include "qai_forge/scheduler/PredictiveModelPool.h"

namespace qai_forge {

namespace {

// Convert public GenerateOptions → internal SchedulerInvokeOptions.
// Sets kind, priority, and skip_summarization_middleware automatically.
scheduler::SchedulerInvokeOptions toSchedulerOptions(
    const GenerateOptions& opts,
    scheduler::JobKind kind) {
    scheduler::SchedulerInvokeOptions sched;
    sched.response_id             = opts.response_id;
    sched.previous_response_id    = opts.previous_response_id;
    sched.session_id              = opts.session_id;
    sched.tool_output_submission  = opts.tool_output_submission;
    sched.allow_tool_chain_fallback = opts.allow_tool_chain_fallback;
    sched.use_response_history    = opts.use_response_history;
    sched.response_history        = opts.response_history;
    sched.summary_content         = opts.summary_content;
    sched.summary_token_count     = opts.summary_token_count;
    sched.facts                   = opts.facts;
    sched.evicted_message_count   = opts.evicted_message_count;
    sched.kind                    = kind;
    sched.skip_summarization_middleware = true;
    return sched;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Impl — Pimpl struct holding scheduler-internal members
//
// Keeps scheduler headers out of the public QaiForge.h API.
// ─────────────────────────────────────────────────────────────────────────────
struct QaiForge::Impl {
    // Predictive AI pool — manages warm PredictiveModelRuntime instances.
    // Initialized with sensible defaults; max_active_models=3 allows three
    // different predictive models to be resident simultaneously.
    scheduler::PredictiveModelPool predictive_pool{
        scheduler::WarmModelPoolConfig{
            /* max_active_models          = */ 3,
            /* max_concurrent_model_loads = */ 1,
            /* idle_timeout               = */ std::chrono::minutes(5),
            /* blocked_admission_timeout  = */ std::chrono::seconds(30),
            /* tool_response_timeout      = */ std::chrono::seconds(30),
            /* max_queue_depth_per_model  = */ 0,
            /* memory_headroom_mb         = */ 1024,
        }
    };
};

// ─────────────────────────────────────────────────────────────────────────────
// Singleton
// ─────────────────────────────────────────────────────────────────────────────
QaiForge& QaiForge::getInstance() {
    static QaiForge instance;
    return instance;
}

QaiForge::QaiForge()
    : impl_(std::make_unique<Impl>())
{}

QaiForge::~QaiForge() = default;

// ─────────────────────────────────────────────────────────────────────────────
// Generative AI
// ─────────────────────────────────────────────────────────────────────────────
StandardResponse QaiForge::generate(
    const CreateChatCompletionRequest& request,
    const GenerateOptions& options) {
    return scheduler::ModelScheduler::getInstance().runBlocking(
        request,
        toSchedulerOptions(options, scheduler::JobKind::HTTP_NON_STREAMING));
}

void QaiForge::generateStream(
    const CreateChatCompletionRequest& request,
    StreamCallbacks callbacks,
    const GenerateOptions& options) {
    scheduler::ModelScheduler::getInstance().runStreamingAsync(
        request,
        std::move(callbacks),
        toSchedulerOptions(options, scheduler::JobKind::HTTP_STREAMING));
}

// ─────────────────────────────────────────────────────────────────────────────
// Predictive AI
//
// Phase 4: routes through PredictiveModelPool instead of the deprecated
// PredictiveAIOrchestrator singleton. The pool manages:
//   - Multiple models loaded simultaneously (one PredictiveModelRuntime each)
//   - LRU eviction when max_active_models is exceeded
//   - Idle-timeout eviction for unused models
//   - Per-model serialization (same model) + cross-model concurrency
// ─────────────────────────────────────────────────────────────────────────────
TensorInferenceResponse QaiForge::infer(const TensorInferenceRequest& request) {
    return impl_->predictive_pool.infer(request);
}

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────
void QaiForge::start() {
    // Start the generative scheduler.
    scheduler::ModelScheduler::getInstance().start();
    // PredictiveModelPool is lazy — models are loaded on first infer() call.
    // No explicit start() needed.
}

void QaiForge::shutdown(bool force) {
    // Shut down the generative scheduler.
    scheduler::ModelScheduler::getInstance().shutdown(force);
    // Shut down all resident predictive models.
    impl_->predictive_pool.stop(force);
}

bool QaiForge::cancel(const std::string& response_id) {
    return scheduler::ModelScheduler::getInstance().cancelResponse(response_id);
}

} // namespace qai_forge
