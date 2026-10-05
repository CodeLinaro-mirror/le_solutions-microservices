// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/QaiForge.h"

#include "qai_forge/backend/BackendFactory.h"
#include "qai_forge/plugin/PluginLoader.h"
#include "qai_forge/plugin/PluginRegistry.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/orchestration/PredictiveOrchestrator.h"
#include "qai_forge/scheduler/ActiveOperationRegistry.h"
#include "qai_forge/scheduler/InferenceScheduler.h"
#include "qai_forge/scheduler/ToolChainLeaseManager.h"
#include "qai_forge/utils/Logger.h"

#include <algorithm>
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
#include <unordered_map>
#include <utility>
#include <vector>

namespace qai_forge {

namespace {

// nlohmann::json's .value(key, default) only falls back to `default` when
// the key is absent — if the key is present but explicitly null,
// .value<std::string>() throws json::type_error.302. This helper treats an
// explicit null the same as an absent key.
std::string getStringOrDefault(const json& obj,
                                const std::string& key,
                                const std::string& def = "") {
    if (!obj.is_object() || !obj.contains(key) || obj[key].is_null()) {
        return def;
    }
    const json& val = obj[key];
    return val.is_string() ? val.get<std::string>() : def;
}

struct QaiForgeConfig {
    std::chrono::milliseconds tool_response_timeout = std::chrono::seconds(30);
    std::chrono::milliseconds checkpoint_interval = std::chrono::seconds(1);
    std::chrono::milliseconds conversation_memory_ttl = std::chrono::minutes(30);
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
    config.conversation_memory_ttl = parseSecondsEnv(
        "CONVERSATION_MEMORY_TTL_SECONDS",
        config.conversation_memory_ttl);
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

std::string internalMcpLeaseId(const std::string& response_id) {
    return "internal_mcp_" + response_id;
}

bool requestContainsToolOutput(const CreateChatCompletionRequest& request) {
    if (!request.messages.is_array()) {
        return false;
    }

    for (const auto& message : request.messages) {
        if (message.is_object() && getStringOrDefault(message, "role", "") == "tool") {
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
    const nlohmann::ordered_json supported_runtimes =
        BackendFactory::supportedRuntimes();
    const nlohmann::ordered_json& generative_runtimes =
        supported_runtimes["generative"];
    const bool runtime_supported = std::any_of(
        generative_runtimes.begin(),
        generative_runtimes.end(),
        [&runtime](const nlohmann::ordered_json& supported) {
            return supported.is_string() &&
                   supported.get<std::string>() == runtime;
        });
    if (!runtime_supported) {
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
          memory_coordinator_(scheduler_.memoryCoordinator()),
          tool_chains_(scheduler_) {}

    ~Impl() {
        shutdown(false);
    }

    StandardResponse generate(const CreateChatCompletionRequest& request,
                              const GenerateOptions& options) {
        validateSchedulableOrThrow(request, "Blocking");

        scheduler::GenerativeJobContext context = prepareContext(
            request,
            options,
            options.internal_mcp_round
                ? scheduler::JobKind::MCP_ROUND
                : scheduler::JobKind::HTTP_NON_STREAMING);

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

        scheduler::GenerativeJobContext context = prepareContext(
            streaming_request,
            options,
            options.internal_mcp_round
                ? scheduler::JobKind::MCP_ROUND
                : scheduler::JobKind::HTTP_STREAMING);

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

        // Discover and dlopen() every qaiforge_plugin_*.so in the plugin
        // directory (QAI_FORGE_PLUGIN_DIR env var, or the installed default)
        // before anything else runs, so BackendFactory/PluginRegistry
        // are fully populated before the scheduler starts accepting jobs.
        PluginLoader::loadAll();

        // Fail-fast diagnostic: catch backend/orchestrator registration
        // mismatches (e.g. a plugin that registers a backend but forgets
        // its orchestrator) at startup via a logged warning, instead of
        // silently deferring to the first request that happens to hit the
        // misconfigured runtime (see PluginRegistry::validateRegistrations()).
        PluginRegistry::instance().validateRegistrations();

        LOG_INFO("[QaiForge] Available runtimes: "
                 << BackendFactory::supportedRuntimes().dump());

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
        memory_coordinator_->clear();

        std::lock_guard<std::mutex> lock(maintenance_mutex_);
        started_ = false;
    }

    bool cancel(const std::string& response_id) {
        if (response_id.empty()) {
            return false;
        }

        const scheduler::ActiveOperationRegistry::CancelInfo info =
            active_operations_.markCancelled(response_id);
        std::string job_id = info.found ? info.job_id : response_id;

        tool_chains_.closeChain(info.lease_model_id, info.lease_id, info.session_id);
        if (info.memory_turn.has_value()) {
            memory_coordinator_->abortTurn(
                info.memory_turn.value(), scheduler::MemoryTurnState::Cancelled);
        }
        const scheduler::CancelResult result = job_id.empty()
            ? scheduler::CancelResult{}
            : scheduler_.cancel(job_id);
        LOG_INFO("[QaiForge] Cancel result: response=" << response_id
                 << " status=" << cancelStatusToString(result.status)
                 << " message=\"" << result.message << "\"");
        return info.found || result.cancelled();
    }

    bool releaseConversationSubtree(
        const ConversationReference& reference) {
        return memory_coordinator_->releaseSubtree(reference);
    }

    bool releaseConversation(const std::string& namespace_id,
                             const std::string& conversation_id) {
        return memory_coordinator_->releaseConversation(
            namespace_id, conversation_id);
    }

    std::string supportedRuntimes() const {
        return BackendFactory::supportedRuntimes().dump();
    }

private:
    scheduler::GenerativeJobContext prepareContext(
        const CreateChatCompletionRequest& request,
        const GenerateOptions& options,
        scheduler::JobKind kind) {
        tool_chains_.expireStale();

        scheduler::GenerativeJobContext context;
        context.job_id = generatedJobId();
        context.model_id = request.model;
        context.request = request;
        context.caller = options;
        context.kind = kind;
        context.priority = scheduler::JobPriority::ANY_REQUEST;
        context.skip_post_turn_summarization =
            !options.conversation.has_value();

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
        context.execution_session_id = session_id;

        if (options.internal_mcp_round &&
            !options.tool_output_submission &&
            !options.previous_response_id.empty()) {
            context.tool_chain_id = internalMcpLeaseId(response_id);
            context.priority = scheduler::JobPriority::TOOL_CONTINUATION;
            context.tool_continuation = true;
            tool_chains_.renewLease(
                context.model_id,
                context.tool_chain_id,
                config_.tool_response_timeout,
                session_id);
        }

        bool tool_output = options.tool_output_submission ||
            (!options.internal_mcp_round && requestContainsToolOutput(request));
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
                context.execution_session_id = chain.session_id;
                context.priority = scheduler::JobPriority::TOOL_CONTINUATION;
                context.tool_continuation = true;
                if (!tool_chains_.markContinuationQueuedAndRenew(
                        chain.chain_id,
                        context.job_id,
                        chain.model_id,
                        config_.tool_response_timeout,
                        chain.session_id)) {
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
                    context.execution_session_id = session_id;
                    context.priority = scheduler::JobPriority::ANY_REQUEST;
                    context.tool_continuation = false;
                }
            }
        }

        if (!context.tool_continuation &&
            !options.previous_response_id.empty()) {
            context.priority = scheduler::JobPriority::ANY_REQUEST;
        }

        // Conversation-memory turn management (beginTurn/abortTurn/publish)
        // is owned by OrchestratorAdapter::createJob(), which reads
        // context.caller.conversation directly.
        return context;
    }

    scheduler::GenerativeJobPtr createJob(
        scheduler::GenerativeJobContext context,
        scheduler::GenerativeCallbacks callbacks) {
        const std::string model_id = context.model_id;
        const std::string tool_chain_id = context.tool_chain_id;
        // OrchestratorAdapter::createJob() owns beginning the
        // conversation-memory turn and aborting it if prepare() throws.
        try {
            scheduler::GenerativeJobPtr job =
                BackendFactory::createGenerativeOrchestratorForModel(
                    model_id)
                    ->createJob(
                        std::move(context), std::move(callbacks));
            wrapCallbacks(*job);
            return job;
        } catch (...) {
            tool_chains_.closeChain(model_id, tool_chain_id);
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
            job->session_id,
            job->priority,
        };

        try {
            active_operations_.registerOperation(*job);
            scheduler::GenerativeRuntimeHandle handle = scheduler_.reserve(metadata);
            const scheduler::SubmitResult result = handle.submit(job);
            if (!result.accepted()) {
                throw submitErrorToException(result);
            }
        } catch (...) {
            tool_chains_.closeChain(job->model_id, job->tool_chain_id);
            active_operations_.finishOperation(job->response_id, job->job_id, false);
            if (job->memory_turn.has_value()) {
                memory_coordinator_->abortTurn(
                    job->memory_turn.value(),
                    scheduler::MemoryTurnState::GenerationFailed);
            }
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
        const scheduler::JobKind kind = job.kind;
        const std::string internal_mcp_lease_id =
            kind == scheduler::JobKind::MCP_ROUND
                ? internalMcpLeaseId(response_id)
                : std::string();
        const std::string job_id = job.job_id;

        job.callbacks.on_token = original_callbacks->on_token;
        job.callbacks.on_complete =
            [this,
             original_callbacks,
             response_id,
             model_id,
             session_id,
             previous_chain_id,
             internal_mcp_lease_id,
             job_id,
             kind](const StandardResponse& response) {
                const bool awaiting_internal_tool =
                    kind == scheduler::JobKind::MCP_ROUND &&
                    responseHasToolCalls(response);
                if (kind == scheduler::JobKind::MCP_ROUND) {
                    if (awaiting_internal_tool) {
                        tool_chains_.openLease(
                            model_id,
                            internal_mcp_lease_id,
                            config_.tool_response_timeout,
                            session_id);
                        if (previous_chain_id != internal_mcp_lease_id) {
                            tool_chains_.closeChain(
                                model_id, previous_chain_id, session_id);
                        }
                    } else {
                        tool_chains_.closeChain(model_id, previous_chain_id, session_id);
                        if (previous_chain_id != internal_mcp_lease_id) {
                            tool_chains_.closeChain(
                                model_id, internal_mcp_lease_id, session_id);
                        }
                    }
                } else {
                    handleCompletion(
                        response_id,
                        model_id,
                        session_id,
                        previous_chain_id,
                        response);
                }
                const bool kept_for_resume = active_operations_.finishOperation(
                    response_id,
                    job_id,
                    awaiting_internal_tool);
                if (awaiting_internal_tool && !kept_for_resume) {
                    tool_chains_.closeChain(
                        model_id, internal_mcp_lease_id, session_id);
                }
                if (original_callbacks->on_complete) {
                    original_callbacks->on_complete(response);
                }
            };
        // Aborting the conversation-memory turn on error/cancellation is
        // handled by OrchestratorAdapter's own callback wrapping
        // (underneath original_callbacks here).
        job.callbacks.on_error =
            [this,
             original_callbacks,
             response_id,
             job_id,
             model_id,
             session_id,
             previous_chain_id](const GenAIException& error) {
                tool_chains_.closeChain(model_id, previous_chain_id, session_id);
                active_operations_.finishOperation(response_id, job_id, false);
                if (original_callbacks->on_error) {
                    original_callbacks->on_error(error);
                }
            };
        job.callbacks.on_cancelled =
            [this,
             original_callbacks,
             response_id,
             job_id,
             model_id,
             session_id,
             previous_chain_id]() {
                tool_chains_.closeChain(model_id, previous_chain_id, session_id);
                active_operations_.finishOperation(response_id, job_id, false);
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
            tool_chains_.closeChain(model_id, previous_chain_id, session_id);
            return;
        }

        const std::string new_response_id = response_id.empty()
            ? response.id
            : response_id;
        const std::string new_model_id = response.model.empty()
            ? model_id
            : response.model;
        // Install the replacement lease before releasing the previous chain.
        // ToolChainLeaseManager::openAndLease() opens the new lease before
        // returning, so the old chain can be closed afterward without
        // exposing an eviction-visible lease gap.
        const scheduler::ToolChainEntry chain = tool_chains_.openAndLease(
            new_response_id,
            new_model_id,
            session_id,
            config_.tool_response_timeout);
        tool_chains_.closeChain(model_id, previous_chain_id, session_id);

        LOG_INFO("[QaiForge] Opened tool chain: chain=" << chain.chain_id
                 << " response=" << chain.response_id
                 << " model=" << chain.model_id
                 << " session=" << chain.session_id);
    }

    void expireIdleActiveOperations() {
        const std::vector<std::pair<std::string, std::string>>
            expired_leases =
                active_operations_.expireIdle(config_.conversation_memory_ttl);
        for (const auto& lease : expired_leases) {
            tool_chains_.closeChain(lease.first, lease.second);
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
            tool_chains_.expireStale();
            memory_coordinator_->expireIdle(
                config_.conversation_memory_ttl);
            expireIdleActiveOperations();
            lock.lock();
        }
    }

    QaiForgeConfig config_;
    scheduler::InferenceScheduler& scheduler_;
    std::shared_ptr<scheduler::ConversationMemoryCoordinator>
        memory_coordinator_;
    scheduler::ToolChainLeaseManager tool_chains_;
    scheduler::ActiveOperationRegistry active_operations_;

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

bool QaiForge::releaseConversationSubtree(
    const ConversationReference& reference) {
    return impl_->releaseConversationSubtree(reference);
}

bool QaiForge::releaseConversation(const std::string& namespace_id,
                                   const std::string& conversation_id) {
    return impl_->releaseConversation(namespace_id, conversation_id);
}

void QaiForge::clearSession(const std::string& model_id,
                             const std::string& session_id) {
    if (model_id.empty() || session_id.empty()) {
        return;
    }
    scheduler::InferenceScheduler::getInstance().clearSession(model_id, session_id);
}

std::string QaiForge::supportedRuntimes() const {
    return impl_->supportedRuntimes();
}

} // namespace qai_forge
