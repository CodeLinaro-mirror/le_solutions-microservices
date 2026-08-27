// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/QaiForge.h"

#include "qai_forge/backend/BackendFactory.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/orchestration/PredictiveOrchestrator.h"
#include "qai_forge/scheduler/InferenceScheduler.h"
#include "qai_forge/scheduler/ToolChainTable.h"
#include "qai_forge/utils/Logger.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace qai_forge {

namespace {

struct QaiForgeConfig {
    std::chrono::milliseconds tool_response_timeout = std::chrono::seconds(30);
    std::chrono::milliseconds checkpoint_interval = std::chrono::seconds(1);
};

long parseLongEnv(const char* name, long fallback) {
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }

    try {
        return std::stol(value);
    } catch (...) {
        return fallback;
    }
}

std::chrono::milliseconds parseSecondsEnv(
    const char* name,
    std::chrono::milliseconds fallback) {
    const long parsed = parseLongEnv(name, -1);
    return parsed > 0 ? std::chrono::seconds(parsed) : fallback;
}

QaiForgeConfig configFromEnvironment() {
    QaiForgeConfig config;
    config.tool_response_timeout = parseSecondsEnv(
        "TOOL_RESPONSE_TIMEOUT_SECONDS",
        config.tool_response_timeout);
    return config;
}

std::string generatedJobId() {
    static std::atomic<uint64_t> next_id{1};
    return "sched_job_" + std::to_string(next_id.fetch_add(1));
}

std::string generatedPredictiveJobId() {
    static std::atomic<uint64_t> next_id{1};
    return "predict_job_" + std::to_string(next_id.fetch_add(1));
}

bool requestContainsToolOutput(const CreateChatCompletionRequest& request) {
    if (!request.messages.is_array()) {
        return false;
    }

    for (const auto& message : request.messages) {
        if (message.is_object() && message.value("role", "") == "tool") {
            return true;
        }
    }
    return false;
}

const char* kindToString(scheduler::JobKind kind) {
    switch (kind) {
        case scheduler::JobKind::HTTP_NON_STREAMING:
            return "http_non_streaming";
        case scheduler::JobKind::HTTP_STREAMING:
            return "http_streaming";
        case scheduler::JobKind::WEBSOCKET:
            return "websocket";
        case scheduler::JobKind::MCP_ROUND:
            return "mcp_round";
        case scheduler::JobKind::INTERNAL_SUMMARIZATION:
            return "internal_summarization";
    }
    return "unknown";
}

const char* priorityToString(scheduler::JobPriority priority) {
    switch (priority) {
        case scheduler::JobPriority::CONTROL:
            return "control";
        case scheduler::JobPriority::TOOL_CONTINUATION:
            return "tool_continuation";
        case scheduler::JobPriority::ANY_REQUEST:
            return "any_request";
    }
    return "unknown";
}

const char* cancelStatusToString(scheduler::CancelStatus status) {
    switch (status) {
        case scheduler::CancelStatus::QUEUED_CANCELLED:
            return "queued_cancelled";
        case scheduler::CancelStatus::RUNNING_CANCELLED:
            return "running_cancelled";
        case scheduler::CancelStatus::NOT_FOUND:
            return "not_found";
    }
    return "unknown";
}

int httpStatusForSubmitStatus(scheduler::SubmitStatus status) {
    switch (status) {
        case scheduler::SubmitStatus::QUEUED:
            return 202;
        case scheduler::SubmitStatus::REJECTED_PREVIOUS_RESPONSE_NOT_FOUND:
            return 404;
        case scheduler::SubmitStatus::REJECTED_TOOL_RESPONSE_TIMEOUT:
            return 408;
        case scheduler::SubmitStatus::REJECTED_QUEUE_FULL:
            return 503;
        case scheduler::SubmitStatus::REJECTED_ADMISSION_TIMEOUT:
        case scheduler::SubmitStatus::REJECTED_SHUTTING_DOWN:
            return 503;
        case scheduler::SubmitStatus::REJECTED_MODEL_NOT_FOUND:
            return 404;
    }
    return 500;
}

scheduler::SubmitResult rejected(scheduler::SubmitStatus status,
                                 const std::string& job_id,
                                 std::string message) {
    scheduler::SubmitResult result;
    result.status = status;
    result.job_id = job_id;
    result.message = std::move(message);
    return result;
}

GenAIException submitErrorToException(const scheduler::SubmitResult& result) {
    GenAIErrorCode code = GenAIErrorCode::INTERNAL_ERROR;
    switch (result.status) {
        case scheduler::SubmitStatus::REJECTED_MODEL_NOT_FOUND:
            code = GenAIErrorCode::MODEL_NOT_FOUND;
            break;
        case scheduler::SubmitStatus::REJECTED_PREVIOUS_RESPONSE_NOT_FOUND:
        case scheduler::SubmitStatus::REJECTED_TOOL_RESPONSE_TIMEOUT:
            code = GenAIErrorCode::TOOL_RESPONSE_TIMEOUT;
            break;
        case scheduler::SubmitStatus::REJECTED_QUEUE_FULL:
        case scheduler::SubmitStatus::REJECTED_ADMISSION_TIMEOUT:
            code = GenAIErrorCode::INSUFFICIENT_MEMORY;
            break;
        case scheduler::SubmitStatus::REJECTED_SHUTTING_DOWN:
            code = GenAIErrorCode::HARDWARE_UNAVAILABLE;
            break;
        case scheduler::SubmitStatus::QUEUED:
            break;
    }

    const std::string message = result.message.empty()
        ? "Inference scheduler rejected request"
        : result.message;
    return GenAIException(code, message, httpStatusForSubmitStatus(result.status));
}

void validateSchedulableOrThrow(const CreateChatCompletionRequest& request,
                                const char* request_kind) {
    if (request.model.empty()) {
        throw GenAIException(
            GenAIErrorCode::INVALID_REQUEST,
            "Request cannot be scheduled: request model is empty",
            400);
    }

    auto& config_manager = ModelConfigManager::getInstance();
    if (!config_manager.validateModel(request.model)) {
        LOG_WARN("[QaiForge] " << request_kind
                 << " request rejected: model=" << request.model
                 << " reason=\"model is not registered\"");
        throw GenAIException(
            GenAIErrorCode::MODEL_NOT_FOUND,
            "Model '" + request.model +
                "' not found. Check /v1/models for available models.",
            404);
    }

    const std::string runtime = config_manager.getRuntime(request.model);
    if (runtime != "genie" && runtime != "litert_lm" &&
        runtime != "llamacpp") {
        throw GenAIException(
            GenAIErrorCode::INVALID_REQUEST,
            "Request cannot be scheduled: runtime '" + runtime +
                "' is not supported by the scheduler",
            400);
    }
}

template <typename PromiseT, typename Setter>
void setPromiseOnce(const std::shared_ptr<PromiseT>& promise,
                    const std::shared_ptr<std::mutex>& mutex,
                    const std::shared_ptr<bool>& completed,
                    Setter&& setter) {
    std::lock_guard<std::mutex> lock(*mutex);
    if (*completed) {
        return;
    }
    *completed = true;
    setter(*promise);
}

bool responseHasToolCalls(const StandardResponse& response) {
    return response.finish_reason == "tool_calls" &&
           response.tool_calls.has_value() &&
           !response.tool_calls.value().empty();
}

} // namespace

struct QaiForge::Impl {
    explicit Impl(QaiForgeConfig config = configFromEnvironment())
        : config_(std::move(config)),
          scheduler_(scheduler::InferenceScheduler::getInstance()),
          memory_coordinator_(scheduler_.memoryCoordinator()) {}

    ~Impl() {
        shutdown(false);
    }

    StandardResponse generate(const CreateChatCompletionRequest& request,
                              const GenerateOptions& options) {
        validateSchedulableOrThrow(request, "Blocking");

        // Task 4 step ⓪ seam: await session memory readiness before context reads.
        scheduler::GenerativeJobContext context = prepareContext(
            request, options, scheduler::JobKind::HTTP_NON_STREAMING);

        auto promise = std::make_shared<std::promise<StandardResponse>>();
        auto future = promise->get_future();
        auto callback_mutex = std::make_shared<std::mutex>();
        auto completed = std::make_shared<bool>(false);

        scheduler::GenerativeCallbacks callbacks;
        callbacks.on_complete =
            [promise, callback_mutex, completed, job_id = context.job_id](
                const StandardResponse& response) {
                LOG_INFO("[QaiForge] Blocking request completed: job=" << job_id
                         << " finish_reason=" << response.finish_reason);
                setPromiseOnce(
                    promise,
                    callback_mutex,
                    completed,
                    [&response](std::promise<StandardResponse>& value) {
                        value.set_value(response);
                    });
            };
        callbacks.on_error =
            [promise, callback_mutex, completed, job_id = context.job_id](
                const GenAIException& error) {
                LOG_WARN("[QaiForge] Blocking request failed: job=" << job_id
                         << " status=" << error.http_status
                         << " message=\"" << error.message << "\"");
                setPromiseOnce(
                    promise,
                    callback_mutex,
                    completed,
                    [&error](std::promise<StandardResponse>& value) {
                        value.set_exception(std::make_exception_ptr(error));
                    });
            };
        callbacks.on_cancelled =
            [promise, callback_mutex, completed, job_id = context.job_id]() {
                LOG_WARN("[QaiForge] Blocking request cancelled: job=" << job_id);
                setPromiseOnce(
                    promise,
                    callback_mutex,
                    completed,
                    [](std::promise<StandardResponse>& value) {
                        value.set_exception(std::make_exception_ptr(
                            GenAIException(
                                GenAIErrorCode::INTERNAL_ERROR,
                                "Scheduled inference request was cancelled",
                                499)));
                    });
            };

        scheduler::GenerativeJobPtr job = createJob(
            std::move(context), std::move(callbacks));
        submitOrThrow(job, options.previous_response_id);
        return future.get();
    }

    void generateStream(const CreateChatCompletionRequest& request,
                        StreamCallbacks callbacks,
                        const GenerateOptions& options) {
        validateSchedulableOrThrow(request, "Streaming");
        CreateChatCompletionRequest streaming_request = request;
        streaming_request.stream = true;

        // Task 4 step ⓪ seam: await session memory readiness before context reads.
        scheduler::GenerativeJobContext context = prepareContext(
            streaming_request,
            options,
            scheduler::JobKind::HTTP_STREAMING);

        scheduler::GenerativeCallbacks inference_callbacks;
        inference_callbacks.on_token = std::move(callbacks.onToken);
        inference_callbacks.on_complete =
            [user_callback = std::move(callbacks.onComplete),
             job_id = context.job_id](const StandardResponse& response) {
                LOG_INFO("[QaiForge] Streaming request completed: job=" << job_id
                         << " finish_reason=" << response.finish_reason);
                if (user_callback) {
                    user_callback(response);
                }
            };
        inference_callbacks.on_error =
            [user_callback = std::move(callbacks.onError),
             job_id = context.job_id](const GenAIException& error) {
                LOG_WARN("[QaiForge] Streaming request failed: job=" << job_id
                         << " status=" << error.http_status
                         << " message=\"" << error.message << "\"");
                if (user_callback) {
                    user_callback(error);
                }
            };
        inference_callbacks.on_cancelled =
            [user_callback = std::move(callbacks.onCancelled),
             job_id = context.job_id]() {
                LOG_WARN("[QaiForge] Streaming request cancelled: job=" << job_id);
                if (user_callback) {
                    user_callback();
                }
            };

        scheduler::GenerativeJobPtr job = createJob(
            std::move(context), std::move(inference_callbacks));
        submitOrThrow(job, options.previous_response_id);
    }

    TensorInferenceResponse infer(const TensorInferenceRequest& request) {
        const std::string job_id = request.request_id.empty()
            ? generatedPredictiveJobId()
            : request.request_id;
        scheduler::PredictiveJobContext context{
            job_id,
            request.model,
            request,
        };

        auto promise =
            std::make_shared<std::promise<TensorInferenceResponse>>();
        auto future = promise->get_future();
        auto callback_mutex = std::make_shared<std::mutex>();
        auto completed = std::make_shared<bool>(false);

        scheduler::PredictiveCallbacks callbacks;
        callbacks.on_complete =
            [promise, callback_mutex, completed](
                const TensorInferenceResponse& response) {
                setPromiseOnce(
                    promise,
                    callback_mutex,
                    completed,
                    [&response](std::promise<TensorInferenceResponse>& value) {
                        value.set_value(response);
                    });
            };
        callbacks.on_error =
            [promise, callback_mutex, completed](const GenAIException& error) {
                setPromiseOnce(
                    promise,
                    callback_mutex,
                    completed,
                    [&error](std::promise<TensorInferenceResponse>& value) {
                        value.set_exception(std::make_exception_ptr(error));
                    });
            };

        scheduler::PredictiveJobPtr job =
            BackendFactory::createPredictiveOrchestrator()->createJob(
                std::move(context),
                std::move(callbacks));
        scheduler::PredictiveScheduleMetadata metadata{
            job->job_id,
            job->model_id,
        };
        scheduler::PredictiveRuntimeHandle handle = scheduler_.reserve(metadata);
        const scheduler::SubmitResult result = handle.submit(job);
        if (!result.accepted()) {
            throw submitErrorToException(result);
        }
        return future.get();
    }

    void start() {
        std::lock_guard<std::mutex> lock(maintenance_mutex_);
        if (started_) {
            return;
        }
        shutdown_requested_ = false;
        scheduler_.start();
        maintenance_thread_ = std::thread(&Impl::maintenanceLoop, this);
        started_ = true;
    }

    void shutdown(bool force) {
        bool should_join = false;
        {
            std::lock_guard<std::mutex> lock(maintenance_mutex_);
            shutdown_requested_ = true;
            should_join = maintenance_thread_.joinable() &&
                          maintenance_thread_.get_id() !=
                              std::this_thread::get_id();
        }
        maintenance_cv_.notify_all();
        if (should_join) {
            maintenance_thread_.join();
        }

        // Runtime callbacks capture this Impl. Stop and join every runtime
        // before any callback-owned QaiForge state can be destroyed.
        scheduler_.shutdown(force);

        std::lock_guard<std::mutex> lock(maintenance_mutex_);
        started_ = false;
    }

    bool cancel(const std::string& response_id) {
        if (response_id.empty()) {
            return false;
        }
        const scheduler::CancelResult result = scheduler_.cancel(response_id);
        LOG_INFO("[QaiForge] Cancel result: response=" << response_id
                 << " status=" << cancelStatusToString(result.status)
                 << " message=\"" << result.message << "\"");
        return result.cancelled();
    }

    std::optional<ConversationMemoryUpdate> awaitConversationMemory(
        const std::string& memory_key) {
        return scheduler_.awaitConversationMemory(memory_key);
    }

    bool enqueueStoreTask(std::string idempotency_key,
                          std::function<void()> task) {
        return scheduler_.enqueueStoreTask(
            std::move(idempotency_key),
            std::move(task));
    }

private:
    scheduler::GenerativeJobContext prepareContext(
        const CreateChatCompletionRequest& request,
        const GenerateOptions& options,
        scheduler::JobKind kind) {
        expireStaleToolChains();

        scheduler::GenerativeJobContext context;
        context.job_id = options.response_id.empty()
            ? generatedJobId()
            : options.response_id;
        context.model_id = request.model;
        context.request = request;
        context.caller = options;
        context.kind = kind;
        context.priority = scheduler::JobPriority::ANY_REQUEST;
        context.skip_post_turn_summarization = false;

        const std::string response_id = options.response_id.empty()
            ? context.job_id
            : options.response_id;
        std::string session_id;
        if (!options.session_id.empty()) {
            session_id = options.session_id;
        } else if (request.user.has_value() && !request.user->empty()) {
            session_id = request.user.value();
        } else if (!options.previous_response_id.empty()) {
            session_id = options.previous_response_id;
        } else {
            session_id = response_id;
        }
        context.request.user = session_id;

        bool tool_output = options.tool_output_submission ||
                           requestContainsToolOutput(request);
        if (tool_output && !options.previous_response_id.empty()) {
            const scheduler::ToolChainResolveResult resolved =
                tool_chains_.resolveByPreviousResponseId(
                    options.previous_response_id);
            if (resolved.status == scheduler::ToolChainResolveStatus::Expired) {
                if (!options.allow_tool_chain_fallback) {
                    throw submitErrorToException(rejected(
                        scheduler::SubmitStatus::REJECTED_TOOL_RESPONSE_TIMEOUT,
                        context.job_id,
                        resolved.message));
                }
                tool_output = false;
            }
            if (tool_output &&
                (resolved.status ==
                     scheduler::ToolChainResolveStatus::NotFound ||
                 !resolved.entry.has_value())) {
                if (!options.allow_tool_chain_fallback) {
                    throw submitErrorToException(rejected(
                        scheduler::SubmitStatus::
                            REJECTED_PREVIOUS_RESPONSE_NOT_FOUND,
                        context.job_id,
                        resolved.message));
                }
                tool_output = false;
            }

            if (tool_output) {
                const scheduler::ToolChainEntry& chain = resolved.entry.value();
                context.tool_chain_id = chain.chain_id;
                context.model_id = chain.model_id;
                context.request.model = chain.model_id;
                context.request.user = chain.session_id;
                context.priority = scheduler::JobPriority::TOOL_CONTINUATION;
                context.tool_continuation = true;
                if (!tool_chains_.markContinuationQueued(
                        chain.chain_id,
                        context.job_id,
                        config_.tool_response_timeout)) {
                    if (!options.allow_tool_chain_fallback) {
                        throw submitErrorToException(rejected(
                            scheduler::SubmitStatus::
                                REJECTED_TOOL_RESPONSE_TIMEOUT,
                            context.job_id,
                            "Tool response window expired for previous_response_id '" +
                                options.previous_response_id + "'"));
                    }
                    context.tool_chain_id.clear();
                    context.model_id = request.model;
                    context.request.model = request.model;
                    context.request.user = session_id;
                    context.priority = scheduler::JobPriority::ANY_REQUEST;
                    context.tool_continuation = false;
                } else {
                    scheduler_.renewToolLease(
                        chain.model_id,
                        chain.chain_id,
                        config_.tool_response_timeout);
                }
            }
        }

        if (!context.tool_continuation &&
            !options.previous_response_id.empty()) {
            context.priority = scheduler::JobPriority::ANY_REQUEST;
        }

        const std::string final_session_id =
            context.request.user.value_or(session_id);
        const bool has_explicit_memory_keys =
            !context.caller.conversation_memory_read_key.empty() ||
            !context.caller.conversation_memory_write_key.empty();
        const std::string conversation_memory_read_key =
            has_explicit_memory_keys
                ? context.caller.conversation_memory_read_key
                : final_session_id;
        context.conversation_memory_write_key =
            !context.caller.conversation_memory_write_key.empty()
                ? context.caller.conversation_memory_write_key
                : final_session_id;
        ConversationMemoryUpdate input_memory;
        input_memory.summary_content = context.caller.summary_content;
        input_memory.summary_token_count = context.caller.summary_token_count;
        input_memory.facts = context.caller.facts;
        input_memory.evicted_message_count =
            context.caller.evicted_message_count;
        if (!conversation_memory_read_key.empty()) {
            memory_coordinator_->seedIfAbsent(
                conversation_memory_read_key,
                input_memory);
            memory_coordinator_->awaitReady(conversation_memory_read_key);
            const std::optional<ConversationMemoryUpdate> committed_memory =
                memory_coordinator_->committedSnapshot(
                    conversation_memory_read_key);
            if (committed_memory.has_value()) {
                context.caller.summary_content =
                    committed_memory->summary_content;
                context.caller.summary_token_count =
                    committed_memory->summary_token_count;
                context.caller.facts = committed_memory->facts;
                if (!context.caller.response_history_is_pruned) {
                    context.caller.evicted_message_count =
                        committed_memory->evicted_message_count;
                }
            }
        }
        return context;
    }

    scheduler::GenerativeJobPtr createJob(
        scheduler::GenerativeJobContext context,
        scheduler::GenerativeCallbacks callbacks) {
        const std::string model_id = context.model_id;
        const std::string tool_chain_id = context.tool_chain_id;
        const std::string conversation_memory_key =
            context.conversation_memory_write_key;
        try {
            scheduler::GenerativeJobPtr job =
                BackendFactory::createGenerativeOrchestratorForModel(
                    model_id)
                    ->createJob(
                        std::move(context), std::move(callbacks));
            job->conversation_memory_key = conversation_memory_key;
            wrapCallbacks(*job);
            return job;
        } catch (...) {
            closeChainIfPresent(model_id, tool_chain_id);
            throw;
        }
    }

    void submitOrThrow(const scheduler::GenerativeJobPtr& job,
                       const std::string& previous_response_id) {
        LOG_INFO("[QaiForge] Submitting request: job=" << job->job_id
                 << " response=" << job->response_id
                 << " model=" << job->model_id
                 << " session=" << job->session_id
                 << " previous=" << previous_response_id
                 << " kind=" << kindToString(job->kind)
                 << " priority=" << priorityToString(job->priority)
                 << " tool_continuation="
                 << (!job->tool_chain_id.empty() ? "true" : "false"));

        scheduler::GenerativeScheduleMetadata metadata{
            job->job_id,
            job->model_id,
            job->priority,
        };

        try {
            scheduler::GenerativeRuntimeHandle handle = scheduler_.reserve(metadata);
            const scheduler::SubmitResult result = handle.submit(job);
            if (!result.accepted()) {
                throw submitErrorToException(result);
            }
        } catch (...) {
            closeChainIfPresent(job->model_id, job->tool_chain_id);
            throw;
        }
    }

    void wrapCallbacks(scheduler::GenerativeJob& job) {
        auto original_callbacks = std::make_shared<scheduler::GenerativeCallbacks>(
            std::move(job.callbacks));
        const std::string response_id = job.response_id;
        const std::string model_id = job.model_id;
        const std::string session_id = job.session_id;
        const std::string previous_chain_id = job.tool_chain_id;

        job.callbacks.on_token = original_callbacks->on_token;
        job.callbacks.on_complete =
            [this,
             original_callbacks,
             response_id,
             model_id,
             session_id,
             previous_chain_id](const StandardResponse& response) {
                handleCompletion(
                    response_id,
                    model_id,
                    session_id,
                    previous_chain_id,
                    response);
                if (original_callbacks->on_complete) {
                    original_callbacks->on_complete(response);
                }
            };
        job.callbacks.on_error =
            [this,
             original_callbacks,
             model_id,
             previous_chain_id](const GenAIException& error) {
                closeChainIfPresent(model_id, previous_chain_id);
                if (original_callbacks->on_error) {
                    original_callbacks->on_error(error);
                }
            };
        job.callbacks.on_cancelled =
            [this, original_callbacks, model_id, previous_chain_id]() {
                closeChainIfPresent(model_id, previous_chain_id);
                if (original_callbacks->on_cancelled) {
                    original_callbacks->on_cancelled();
                }
            };
    }

    void handleCompletion(const std::string& response_id,
                          const std::string& model_id,
                          const std::string& session_id,
                          const std::string& previous_chain_id,
                          const StandardResponse& response) {
        if (!responseHasToolCalls(response)) {
            closeChainIfPresent(model_id, previous_chain_id);
            return;
        }

        const std::string new_response_id = response_id.empty()
            ? response.id
            : response_id;
        const std::string new_model_id = response.model.empty()
            ? model_id
            : response.model;
        const scheduler::ToolChainEntry chain = tool_chains_.open(
            new_response_id,
            new_model_id,
            session_id,
            config_.tool_response_timeout);

        // Install the replacement lease before releasing the previous chain.
        // WarmModelPool swaps its chain id under one mutex, so closing the old
        // id afterward cannot expose an eviction-visible lease gap.
        scheduler_.openToolLease(
            chain.model_id,
            chain.chain_id,
            config_.tool_response_timeout);
        closeChainIfPresent(model_id, previous_chain_id);

        LOG_INFO("[QaiForge] Opened tool chain: chain=" << chain.chain_id
                 << " response=" << chain.response_id
                 << " model=" << chain.model_id
                 << " session=" << chain.session_id);
    }

    void closeChainIfPresent(const std::string& model_id,
                             const std::string& chain_id) {
        if (chain_id.empty()) {
            return;
        }
        tool_chains_.close(chain_id);
        scheduler_.closeToolLease(model_id, chain_id);
    }

    void expireStaleToolChains() {
        const std::vector<scheduler::ToolChainEntry> expired =
            tool_chains_.expireStale();
        for (const scheduler::ToolChainEntry& chain : expired) {
            scheduler_.closeToolLease(chain.model_id, chain.chain_id);
        }
    }

    void maintenanceLoop() {
        std::unique_lock<std::mutex> lock(maintenance_mutex_);
        while (!shutdown_requested_) {
            if (maintenance_cv_.wait_for(
                    lock,
                    config_.checkpoint_interval,
                    [this]() { return shutdown_requested_; })) {
                break;
            }
            lock.unlock();
            expireStaleToolChains();
            lock.lock();
        }
    }

    QaiForgeConfig config_;
    scheduler::InferenceScheduler& scheduler_;
    std::shared_ptr<scheduler::ConversationMemoryCoordinator>
        memory_coordinator_;
    scheduler::ToolChainTable tool_chains_;

    std::mutex maintenance_mutex_;
    std::condition_variable maintenance_cv_;
    std::thread maintenance_thread_;
    bool started_ = false;
    bool shutdown_requested_ = false;
};

QaiForge& QaiForge::getInstance() {
    static QaiForge instance;
    return instance;
}

QaiForge::QaiForge()
    : impl_(std::make_unique<Impl>()) {}

QaiForge::~QaiForge() {
    if (impl_) {
        impl_->shutdown(false);
    }
}

StandardResponse QaiForge::generate(
    const CreateChatCompletionRequest& request,
    const GenerateOptions& options) {
    return impl_->generate(request, options);
}

void QaiForge::generateStream(
    const CreateChatCompletionRequest& request,
    StreamCallbacks callbacks,
    const GenerateOptions& options) {
    impl_->generateStream(request, std::move(callbacks), options);
}

TensorInferenceResponse QaiForge::infer(const TensorInferenceRequest& request) {
    return impl_->infer(request);
}

void QaiForge::start() {
    impl_->start();
}

void QaiForge::shutdown(bool force) {
    impl_->shutdown(force);
}

bool QaiForge::cancel(const std::string& response_id) {
    return impl_->cancel(response_id);
}

std::optional<ConversationMemoryUpdate> QaiForge::awaitConversationMemory(
    const std::string& memory_key) {
    return impl_->awaitConversationMemory(memory_key);
}

bool QaiForge::enqueueStoreTask(std::string idempotency_key,
                               std::function<void()> task) {
    return impl_->enqueueStoreTask(
        std::move(idempotency_key),
        std::move(task));
}

} // namespace qai_forge
