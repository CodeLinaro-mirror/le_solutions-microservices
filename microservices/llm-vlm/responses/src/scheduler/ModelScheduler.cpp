// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "scheduler/ModelScheduler.h"

#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/Logger.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace scheduler {

namespace {

std::chrono::milliseconds effectiveToolTimeout(
    std::chrono::milliseconds ttl) {
    return ttl.count() > 0 ? ttl : std::chrono::seconds(30);
}

std::chrono::milliseconds effectiveCheckpointInterval(
    std::chrono::milliseconds interval) {
    return interval.count() > 0 ? interval : std::chrono::seconds(1);
}

std::string generatedJobId() {
    static std::atomic<uint64_t> next_id{1};
    return "sched_job_" + std::to_string(next_id.fetch_add(1));
}

bool requestContainsToolOutput(const CreateChatCompletionRequest& request) {
    if (!request.messages.is_array()) {
        return false;
    }

    for (const auto& message : request.messages) {
        if (!message.is_object()) {
            continue;
        }
        if (message.value("role", "") == "tool") {
            return true;
        }
    }
    return false;
}

const char* kindToString(JobKind kind) {
    switch (kind) {
        case JobKind::HTTP_NON_STREAMING:
            return "http_non_streaming";
        case JobKind::HTTP_STREAMING:
            return "http_streaming";
        case JobKind::WEBSOCKET:
            return "websocket";
        case JobKind::MCP_ROUND:
            return "mcp_round";
        case JobKind::INTERNAL_SUMMARIZATION:
            return "internal_summarization";
    }
    return "unknown";
}

ModelSchedulerConfig normalizedConfig(ModelSchedulerConfig config) {
    config.tool_response_timeout =
        effectiveToolTimeout(config.tool_response_timeout);
    config.checkpoint_interval =
        effectiveCheckpointInterval(config.checkpoint_interval);
    if (config.pool_config.tool_response_timeout.count() <= 0) {
        config.pool_config.tool_response_timeout =
            config.tool_response_timeout;
    }
    return config;
}

size_t parseSizeEnv(const char* name, size_t fallback) {
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }

    try {
        unsigned long long parsed = std::stoull(value);
        if (parsed > static_cast<unsigned long long>(
                         std::numeric_limits<size_t>::max())) {
            return fallback;
        }
        return static_cast<size_t>(parsed);
    } catch (...) {
        return fallback;
    }
}

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
    if (parsed <= 0) {
        return fallback;
    }
    return std::chrono::seconds(parsed);
}

RunningCancelMode parseRunningCancelModeEnv(RunningCancelMode fallback) {
    const char* value = std::getenv("RESPONSES_RUNNING_CANCEL_MODE");
    if (!value || value[0] == '\0') {
        return fallback;
    }

    const std::string mode(value);
    if (mode == "hard" || mode == "HARD") {
        return RunningCancelMode::HARD;
    }

    return RunningCancelMode::SOFT;
}

const char* runningCancelModeToString(RunningCancelMode mode) {
    switch (mode) {
        case RunningCancelMode::SOFT:
            return "soft";
        case RunningCancelMode::HARD:
            return "hard";
    }
    return "soft";
}

ModelSchedulerConfig configFromEnvironment() {
    ModelSchedulerConfig config;
    config.pool_config.max_active_models =
        parseSizeEnv("MAX_ACTIVE_MODELS",
                     config.pool_config.max_active_models);
    config.pool_config.max_concurrent_model_loads =
        parseSizeEnv("MAX_CONCURRENT_MODEL_LOADS",
                     config.pool_config.max_concurrent_model_loads);
    config.pool_config.model_residency_ttl =
        parseSecondsEnv("MODEL_RESIDENCY_TTL_SECONDS",
                        config.pool_config.model_residency_ttl);

    config.tool_response_timeout =
        parseSecondsEnv("TOOL_RESPONSE_TIMEOUT_SECONDS",
                        config.tool_response_timeout);
    config.pool_config.tool_response_timeout =
        config.tool_response_timeout;
    config.pool_config.running_cancel_mode =
        parseRunningCancelModeEnv(config.pool_config.running_cancel_mode);

    return config;
}

const char* statusToString(SubmitStatus status) {
    switch (status) {
        case SubmitStatus::QUEUED:
            return "queued";
        case SubmitStatus::REJECTED_MODEL_NOT_FOUND:
            return "rejected_model_not_found";
        case SubmitStatus::REJECTED_QUEUE_FULL:
            return "rejected_queue_full";
        case SubmitStatus::REJECTED_ADMISSION_TIMEOUT:
            return "rejected_admission_timeout";
        case SubmitStatus::REJECTED_SHUTTING_DOWN:
            return "rejected_shutting_down";
        case SubmitStatus::REJECTED_PREVIOUS_RESPONSE_NOT_FOUND:
            return "rejected_previous_response_not_found";
        case SubmitStatus::REJECTED_TOOL_RESPONSE_TIMEOUT:
            return "rejected_tool_response_timeout";
    }
    return "unknown";
}

const char* priorityToString(JobPriority priority) {
    switch (priority) {
        case JobPriority::CONTROL:
            return "control";
        case JobPriority::READY_TOOL_CONT:
            return "ready_tool_cont";
        case JobPriority::SESSION_CONT:
            return "session_cont";
        case JobPriority::NEW_REQUEST:
            return "new_request";
    }
    return "unknown";
}

const char* cancelStatusToString(CancelStatus status) {
    switch (status) {
        case CancelStatus::QUEUED_CANCELLED:
            return "queued_cancelled";
        case CancelStatus::RUNNING_CANCELLED:
            return "running_cancelled";
        case CancelStatus::NOT_FOUND:
            return "not_found";
    }
    return "unknown";
}

void validateSchedulableOrThrow(const CreateChatCompletionRequest& request,
                                const char* request_kind) {
    if (request.model.empty()) {
        const std::string reason = "request model is empty";
        LOG_WARN("[ModelScheduler] " << request_kind
                 << " request rejected before submit: model="
                 << request.model << " reason=\"" << reason << "\"");
        throw GenAIException(
            GenAIErrorCode::INVALID_REQUEST,
            "Request cannot be scheduled: " + reason,
            400);
    }

    auto& config_mgr = ModelConfigManager::getInstance();
    if (!config_mgr.validateModel(request.model)) {
        LOG_WARN("[ModelScheduler] " << request_kind
                 << " request rejected before submit: model="
                 << request.model << " reason=\"model is not registered\"");
        throw GenAIException(
            GenAIErrorCode::MODEL_NOT_FOUND,
            "Model '" + request.model +
                "' not found. Check /v1/models for available models.",
            404);
    }

    const std::string runtime = config_mgr.getRuntime(request.model);
    if (runtime != "genie") {
        const std::string reason =
            "runtime '" + runtime + "' is not supported by the scheduler";
        LOG_WARN("[ModelScheduler] " << request_kind
                 << " request rejected before submit: model="
                 << request.model << " reason=\"" << reason << "\"");
        throw GenAIException(
            GenAIErrorCode::INVALID_REQUEST,
            "Request cannot be scheduled: " + reason,
            400);
    }
}

GenAIException submitErrorToException(const SubmitResult& result) {
    int status = ModelScheduler::httpStatusForSubmitStatus(result.status);
    GenAIErrorCode code = GenAIErrorCode::INTERNAL_ERROR;
    switch (result.status) {
        case SubmitStatus::REJECTED_MODEL_NOT_FOUND:
            code = GenAIErrorCode::MODEL_NOT_FOUND;
            break;
        case SubmitStatus::REJECTED_PREVIOUS_RESPONSE_NOT_FOUND:
        case SubmitStatus::REJECTED_TOOL_RESPONSE_TIMEOUT:
            code = GenAIErrorCode::TOOL_RESPONSE_TIMEOUT;
            break;
        case SubmitStatus::REJECTED_QUEUE_FULL:
        case SubmitStatus::REJECTED_ADMISSION_TIMEOUT:
            code = GenAIErrorCode::INSUFFICIENT_MEMORY;
            break;
        case SubmitStatus::REJECTED_SHUTTING_DOWN:
            code = GenAIErrorCode::HARDWARE_UNAVAILABLE;
            break;
        case SubmitStatus::QUEUED:
            break;
    }

    std::string message = result.message.empty()
                              ? "Model scheduler rejected inference request"
                              : result.message;
    return GenAIException(code, message, status);
}

InferenceJobPtr buildJob(const CreateChatCompletionRequest& request,
                         const SchedulerInvokeOptions& options) {
    auto job = std::make_shared<InferenceJob>();
    job->job_id = options.response_id.empty() ? generatedJobId()
                                              : options.response_id;
    job->response_id = options.response_id.empty() ? job->job_id
                                                   : options.response_id;
    job->previous_response_id = options.previous_response_id;
    if (!options.session_id.empty()) {
        job->session_id = options.session_id;
    } else if (request.user.has_value() && !request.user.value().empty()) {
        job->session_id = request.user.value();
    } else if (!job->previous_response_id.empty()) {
        job->session_id = job->previous_response_id;
    } else {
        job->session_id = job->response_id;
    }
    job->model_id = request.model;
    job->kind = options.kind;
    job->priority = options.priority;
    job->skip_summarization_middleware =
        options.skip_summarization_middleware;
    job->use_response_history = options.use_response_history;
    job->response_history = options.response_history.is_array()
        ? options.response_history
        : json::array();
    job->request = request;
    if (!job->session_id.empty()) {
        job->request.user = job->session_id;
    }

    const bool tool_output =
        options.tool_output_submission || requestContainsToolOutput(request);
    if (tool_output && !job->previous_response_id.empty()) {
        job->is_tool_output_submission = true;
        job->priority = JobPriority::READY_TOOL_CONT;
    } else if (job->priority == JobPriority::NEW_REQUEST &&
               !job->previous_response_id.empty()) {
        job->priority = JobPriority::SESSION_CONT;
    }

    job->is_tool_continuation =
        job->priority == JobPriority::READY_TOOL_CONT ||
        job->is_tool_output_submission;

    return job;
}

void submitOrThrow(ModelScheduler& scheduler, const InferenceJobPtr& job) {
    SubmitResult result = scheduler.submit(job);
    if (!result.accepted()) {
        LOG_WARN("[ModelScheduler] Submit rejected: job="
                 << (job ? job->job_id : std::string("<null>"))
                 << " status=" << static_cast<int>(result.status)
                 << " message=\"" << result.message << "\"");
        throw submitErrorToException(result);
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

} // namespace

ModelScheduler::ModelScheduler(ModelSchedulerConfig config,
                               ModelBackendFactory backend_factory)
    : config_(normalizedConfig(std::move(config))),
      pool_(config_.pool_config, std::move(backend_factory)) {
    LOG_INFO("[ModelScheduler] Configured: max_active_models="
             << config_.pool_config.max_active_models
             << " tool_response_timeout_ms="
             << config_.tool_response_timeout.count()
             << " checkpoint_interval_ms="
             << config_.checkpoint_interval.count()
             << " running_cancel_mode="
             << runningCancelModeToString(
                    config_.pool_config.running_cancel_mode));
}

ModelScheduler::~ModelScheduler() {
    shutdown(false);
}

ModelScheduler& ModelScheduler::getInstance() {
    static ModelScheduler instance(configFromEnvironment());
    return instance;
}

void ModelScheduler::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
        return;
    }

    shutdown_requested_ = false;
    pool_.start();
    started_ = true;
    tick_thread_ = std::thread(&ModelScheduler::tickLoop, this);
    LOG_INFO("[ModelScheduler] Started");
}

StandardResponse ModelScheduler::runBlocking(
    const CreateChatCompletionRequest& request,
    const SchedulerInvokeOptions& options) {
    validateSchedulableOrThrow(request, "Blocking");

    InferenceJobPtr job = buildJob(request, options);
    LOG_INFO("[ModelScheduler] Submitting blocking request: job="
             << job->job_id << " response=" << job->response_id
             << " model=" << job->model_id
             << " session=" << job->session_id
             << " previous=" << job->previous_response_id
             << " kind=" << kindToString(job->kind)
             << " priority=" << priorityToString(job->priority)
             << " tool_output="
             << (job->is_tool_output_submission ? "true" : "false"));

    auto promise = std::make_shared<std::promise<StandardResponse>>();
    auto future = promise->get_future();
    auto mutex = std::make_shared<std::mutex>();
    auto completed = std::make_shared<bool>(false);

    job->callbacks.on_complete =
        [promise, mutex, completed, job_id = job->job_id](
            const StandardResponse& response) {
            LOG_INFO("[ModelScheduler] Blocking request completed: job="
                     << job_id << " finish_reason=" << response.finish_reason
                     << " prompt_tokens=" << response.prompt_tokens
                     << " completion_tokens=" << response.completion_tokens);
            setPromiseOnce(
                promise,
                mutex,
                completed,
                [&response](std::promise<StandardResponse>& p) {
                    p.set_value(response);
                });
        };

    job->callbacks.on_error =
        [promise, mutex, completed, job_id = job->job_id](
            const GenAIException& error) {
            LOG_WARN("[ModelScheduler] Blocking request failed: job="
                     << job_id << " status=" << error.http_status
                     << " message=\"" << error.message << "\"");
            setPromiseOnce(
                promise,
                mutex,
                completed,
                [&error](std::promise<StandardResponse>& p) {
                    p.set_exception(std::make_exception_ptr(error));
                });
        };

    job->callbacks.on_cancelled =
        [promise, mutex, completed, job_id = job->job_id]() {
            LOG_WARN("[ModelScheduler] Blocking request cancelled: job="
                     << job_id);
            setPromiseOnce(
                promise,
                mutex,
                completed,
                [](std::promise<StandardResponse>& p) {
                    p.set_exception(std::make_exception_ptr(
                        GenAIException(
                            GenAIErrorCode::INTERNAL_ERROR,
                            "Scheduled inference request was cancelled",
                            499)));
                });
        };

    submitOrThrow(*this, job);
    return future.get();
}

StandardResponse ModelScheduler::runStreaming(
    const CreateChatCompletionRequest& request,
    std::function<void(const StreamChunk&)> callback,
    const SchedulerInvokeOptions& options) {
    validateSchedulableOrThrow(request, "Streaming");

    SchedulerInvokeOptions effective_options = options;
    if (effective_options.kind == JobKind::HTTP_NON_STREAMING) {
        effective_options.kind = JobKind::HTTP_STREAMING;
    }

    InferenceJobPtr job = buildJob(request, effective_options);
    job->request.stream = true;
    LOG_INFO("[ModelScheduler] Submitting streaming request: job="
             << job->job_id << " response=" << job->response_id
             << " model=" << job->model_id
             << " session=" << job->session_id
             << " previous=" << job->previous_response_id
             << " kind=" << kindToString(job->kind)
             << " priority=" << priorityToString(job->priority)
             << " tool_output="
             << (job->is_tool_output_submission ? "true" : "false"));

    auto promise = std::make_shared<std::promise<StandardResponse>>();
    auto future = promise->get_future();
    auto mutex = std::make_shared<std::mutex>();
    auto completed = std::make_shared<bool>(false);

    job->callbacks.on_token = std::move(callback);
    job->callbacks.on_complete =
        [promise, mutex, completed, job_id = job->job_id](
            const StandardResponse& response) {
            LOG_INFO("[ModelScheduler] Streaming request completed: job="
                     << job_id << " finish_reason=" << response.finish_reason);
            setPromiseOnce(
                promise,
                mutex,
                completed,
                [&response](std::promise<StandardResponse>& p) {
                    p.set_value(response);
                });
        };

    job->callbacks.on_error =
        [promise, mutex, completed, job_id = job->job_id](
            const GenAIException& error) {
            LOG_WARN("[ModelScheduler] Streaming request failed: job="
                     << job_id << " status=" << error.http_status
                     << " message=\"" << error.message << "\"");
            setPromiseOnce(
                promise,
                mutex,
                completed,
                [&error](std::promise<StandardResponse>& p) {
                    p.set_exception(std::make_exception_ptr(error));
                });
        };

    job->callbacks.on_cancelled =
        [promise, mutex, completed, job_id = job->job_id]() {
            LOG_WARN("[ModelScheduler] Streaming request cancelled: job="
                     << job_id);
            setPromiseOnce(
                promise,
                mutex,
                completed,
                [](std::promise<StandardResponse>& p) {
                    p.set_exception(std::make_exception_ptr(
                        GenAIException(
                            GenAIErrorCode::INTERNAL_ERROR,
                            "Scheduled inference request was cancelled",
                            499)));
                });
        };

    submitOrThrow(*this, job);
    return future.get();
}

bool ModelScheduler::cancelResponse(const std::string& response_id) {
    if (response_id.empty()) {
        LOG_INFO("[ModelScheduler] Cancel skipped: response=" << response_id
                 << " reason=\"empty response id\"");
        return false;
    }

    const CancelResult result = cancel(response_id);
    LOG_INFO("[ModelScheduler] Cancel result: response=" << response_id
             << " status=" << cancelStatusToString(result.status)
             << " message=\"" << result.message << "\"");
    return result.cancelled();
}

SubmitResult ModelScheduler::submit(InferenceJobPtr job) {
    if (!job) {
        return reject(
            SubmitStatus::REJECTED_MODEL_NOT_FOUND,
            {},
            "ModelScheduler received null inference job");
    }

    expireStaleToolChains();
    LOG_INFO("[ModelScheduler] Submit: job=" << job->job_id
             << " response=" << job->response_id
             << " model=" << job->model_id
             << " session=" << job->session_id
             << " previous=" << job->previous_response_id
             << " priority=" << priorityToString(job->priority)
             << " tool_output=" << (job->is_tool_output_submission ? "true" : "false"));

    if (job->is_tool_output_submission) {
        const ToolChainResolveResult resolved =
            tool_chains_.resolveByPreviousResponseId(
                job->previous_response_id);

        if (resolved.status == ToolChainResolveStatus::Expired) {
            LOG_WARN("[ModelScheduler] Tool continuation expired: job="
                     << job->job_id << " previous="
                     << job->previous_response_id
                     << " message=\"" << resolved.message << "\"");
            return reject(
                SubmitStatus::REJECTED_TOOL_RESPONSE_TIMEOUT,
                job->job_id,
                resolved.message);
        }

        if (resolved.status == ToolChainResolveStatus::NotFound ||
            !resolved.entry.has_value()) {
            LOG_WARN("[ModelScheduler] Tool continuation unknown: job="
                     << job->job_id << " previous="
                     << job->previous_response_id
                     << " message=\"" << resolved.message << "\"");
            return reject(
                SubmitStatus::REJECTED_PREVIOUS_RESPONSE_NOT_FOUND,
                job->job_id,
                resolved.message);
        }

        prepareToolContinuation(*job, resolved.entry.value());
        LOG_INFO("[ModelScheduler] Tool continuation resolved: job="
                 << job->job_id << " chain=" << job->tool_chain_id
                 << " model=" << job->model_id
                 << " session=" << job->session_id);
        if (!tool_chains_.markContinuationQueued(
                job->tool_chain_id,
                job->job_id,
                config_.tool_response_timeout)) {
            LOG_WARN("[ModelScheduler] Tool continuation lease expired while queueing: job="
                     << job->job_id << " chain=" << job->tool_chain_id);
            return reject(
                SubmitStatus::REJECTED_TOOL_RESPONSE_TIMEOUT,
                job->job_id,
                "Tool response window expired for previous_response_id '" +
                    job->previous_response_id + "'");
        }

        pool_.renewToolLease(
            job->model_id,
            job->tool_chain_id,
            config_.tool_response_timeout);
    }

    wrapCallbacks(*job);
    SubmitResult result = pool_.submit(job);
    if (!result.accepted() && job->is_tool_continuation) {
        closeChainIfPresent(job->model_id, job->tool_chain_id);
    }
    LOG_INFO("[ModelScheduler] Submit result: job=" << job->job_id
             << " status=" << statusToString(result.status)
             << " message=\"" << result.message << "\"");
    return result;
}

CancelResult ModelScheduler::cancel(const std::string& job_id) {
    CancelResult result = pool_.cancel(job_id);
    LOG_INFO("[ModelScheduler] Cancel: job=" << job_id
             << " status=" << cancelStatusToString(result.status)
             << " message=\"" << result.message << "\"");
    return result;
}

void ModelScheduler::checkpoint() {
    expireStaleToolChains();
    pool_.checkpoint();
}

ModelPoolSnapshot ModelScheduler::poolSnapshot() const {
    return pool_.snapshot();
}

std::vector<ToolChainEntry> ModelScheduler::toolChainSnapshot() const {
    return tool_chains_.snapshot();
}

void ModelScheduler::shutdown(bool force) {
    bool should_join = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_requested_ = true;
        should_join = tick_thread_.joinable() &&
                      tick_thread_.get_id() != std::this_thread::get_id();
    }

    cv_.notify_all();

    if (should_join) {
        tick_thread_.join();
    }

    if (!force) {
        checkpoint();
    }

    pool_.stop(force);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        started_ = false;
    }
    LOG_INFO("[ModelScheduler] Stopped force=" << (force ? "true" : "false"));
}

void ModelScheduler::stop(bool force) {
    shutdown(force);
}

int ModelScheduler::httpStatusForSubmitStatus(SubmitStatus status) {
    switch (status) {
        case SubmitStatus::QUEUED:
            return 202;
        case SubmitStatus::REJECTED_PREVIOUS_RESPONSE_NOT_FOUND:
            return 404;
        case SubmitStatus::REJECTED_TOOL_RESPONSE_TIMEOUT:
            return 408;
        case SubmitStatus::REJECTED_QUEUE_FULL:
            return 429;
        case SubmitStatus::REJECTED_ADMISSION_TIMEOUT:
            return 503;
        case SubmitStatus::REJECTED_SHUTTING_DOWN:
            return 503;
        case SubmitStatus::REJECTED_MODEL_NOT_FOUND:
            return 404;
    }

    return 500;
}

SubmitResult ModelScheduler::reject(SubmitStatus status,
                                    const std::string& job_id,
                                    std::string message) const {
    SubmitResult result;
    result.status = status;
    result.job_id = job_id;
    result.message = std::move(message);
    return result;
}

void ModelScheduler::prepareToolContinuation(
    InferenceJob& job,
    const ToolChainEntry& chain) {
    job.tool_chain_id = chain.chain_id;
    job.model_id = chain.model_id;
    job.request.model = chain.model_id;
    job.session_id = chain.session_id;
    if (!job.session_id.empty()) {
        job.request.user = job.session_id;
    }
    job.priority = JobPriority::READY_TOOL_CONT;
    job.is_tool_continuation = true;
    LOG_INFO("[ModelScheduler] Prepared tool continuation: job="
             << job.job_id << " chain=" << job.tool_chain_id
             << " response=" << chain.response_id
             << " model=" << job.model_id
             << " session=" << job.session_id);
}

void ModelScheduler::wrapCallbacks(InferenceJob& job) {
    auto original_callbacks =
        std::make_shared<InferenceCallbacks>(std::move(job.callbacks));

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
            LOG_INFO("[ModelScheduler] Job completed: response="
                     << response_id << " model=" << model_id
                     << " finish_reason=" << response.finish_reason);
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
            LOG_WARN("[ModelScheduler] Job failed: model=" << model_id
                     << " chain=" << previous_chain_id
                     << " status=" << error.http_status
                     << " message=\"" << error.message << "\"");
            closeChainIfPresent(model_id, previous_chain_id);
            if (original_callbacks->on_error) {
                original_callbacks->on_error(error);
            }
        };

    job.callbacks.on_cancelled =
        [this, original_callbacks, model_id, previous_chain_id]() {
            LOG_WARN("[ModelScheduler] Job cancelled: model=" << model_id
                     << " chain=" << previous_chain_id);
            closeChainIfPresent(model_id, previous_chain_id);
            if (original_callbacks->on_cancelled) {
                original_callbacks->on_cancelled();
            }
        };
}

void ModelScheduler::handleCompletion(
    const std::string& response_id,
    const std::string& model_id,
    const std::string& session_id,
    const std::string& previous_chain_id,
    const StandardResponse& response) {
    closeChainIfPresent(model_id, previous_chain_id);

    if (!responseHasToolCalls(response)) {
        return;
    }

    std::string new_response_id = response_id.empty() ? response.id : response_id;
    const std::string new_model_id =
        response.model.empty() ? model_id : response.model;

    ToolChainEntry chain = tool_chains_.open(
        new_response_id,
        new_model_id,
        session_id,
        config_.tool_response_timeout);
    LOG_INFO("[ModelScheduler] Opened tool chain: chain=" << chain.chain_id
             << " response=" << chain.response_id
             << " model=" << chain.model_id
             << " session=" << chain.session_id
             << " ttl_ms=" << config_.tool_response_timeout.count());
    pool_.openToolLease(
        chain.model_id,
        chain.chain_id,
        config_.tool_response_timeout);
}

void ModelScheduler::closeChainIfPresent(const std::string& model_id,
                                         const std::string& chain_id) {
    if (chain_id.empty()) {
        return;
    }

    tool_chains_.close(chain_id);
    pool_.closeToolLease(model_id, chain_id);
    LOG_INFO("[ModelScheduler] Closed tool chain: chain=" << chain_id
             << " model=" << model_id);
}

void ModelScheduler::expireStaleToolChains() {
    const std::vector<ToolChainEntry> expired = tool_chains_.expireStale();
    for (const ToolChainEntry& chain : expired) {
        LOG_WARN("[ModelScheduler] Expired tool chain: chain="
                 << chain.chain_id << " response=" << chain.response_id
                 << " model=" << chain.model_id);
        pool_.closeToolLease(chain.model_id, chain.chain_id);
    }
}

void ModelScheduler::tickLoop() {
    std::unique_lock<std::mutex> lock(mutex_);

    while (!shutdown_requested_) {
        if (cv_.wait_for(lock, config_.checkpoint_interval, [this]() {
                return shutdown_requested_;
            })) {
            break;
        }

        lock.unlock();
        checkpoint();
        lock.lock();
    }
}

bool ModelScheduler::responseHasToolCalls(
    const StandardResponse& response) {
    return response.finish_reason == "tool_calls" &&
           response.tool_calls.has_value() &&
           !response.tool_calls.value().empty();
}

} // namespace scheduler
