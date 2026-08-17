// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/orchestration/LiteRTLMOrchestrator.h"

#include "qai_forge/backend/LiteRTLMBackend.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/Logger.h"

#include <cctype>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

namespace {

std::string generateEventId() {
    static std::mt19937_64 rng(std::random_device{}());
    std::ostringstream oss;
    oss << "litert-lm-" << std::hex << std::setw(16) << std::setfill('0')
        << rng();
    return oss.str();
}

} // namespace

scheduler::GenerativeJobPtr LiteRTLMOrchestrator::createJob(
    scheduler::GenerativeJobContext context,
    scheduler::GenerativeCallbacks callbacks) const {
    json messages = json::array();
    if (context.caller.use_response_history &&
        context.caller.response_history.is_array()) {
        for (auto& message : context.caller.response_history) {
            messages.push_back(std::move(message));
        }
    }
    if (context.request.messages.is_array()) {
        for (auto& message : context.request.messages) {
            messages.push_back(std::move(message));
        }
    }

    json tools = std::move(context.request.tools).value_or(json::array());
    const int evicted = applyContextEviction(
        messages,
        tools,
        ModelConfigManager::getInstance().getContextSize(context.model_id));
    if (evicted > 0) {
        LOG_INFO("[LiteRTLMOrchestrator] Evicted " << evicted
                 << " messages while preparing model=" << context.model_id);
    }

    scheduler::LiteRTLMPreparedRequest prepared;
    prepared.messages = std::move(messages);
    prepared.tools = std::move(tools);
    prepared.generation.max_tokens =
        context.request.max_completion_tokens.value_or(512);
    prepared.generation.temperature =
        context.request.temperature.value_or(0.7f);
    prepared.generation.top_p = context.request.top_p.value_or(0.9f);
    prepared.generation.top_k = context.request.top_k.value_or(40);
    prepared.generation.presence_penalty =
        context.request.presence_penalty.value_or(0.0f);
    prepared.generation.frequency_penalty =
        context.request.frequency_penalty.value_or(0.0f);
    prepared.generation.streaming = static_cast<bool>(callbacks.on_token);

    auto job = std::make_shared<scheduler::GenerativeJob>();
    job->response_id = context.caller.response_id.empty()
        ? context.job_id : std::move(context.caller.response_id);
    job->session_id = context.request.user.value_or(context.job_id);
    job->job_id = std::move(context.job_id);
    job->model_id = std::move(context.model_id);
    job->tool_chain_id = std::move(context.tool_chain_id);
    job->kind = context.kind;
    job->priority = context.priority;
    job->prepared = std::move(prepared);
    job->skip_post_turn_summarization =
        context.skip_post_turn_summarization;
    job->callbacks = std::move(callbacks);
    return job;
}

StandardResponse LiteRTLMOrchestrator::execute(
    scheduler::GenerativeJob& job,
    IGenerativeBackend& backend) const {
    auto* litert_backend = dynamic_cast<LiteRTLMBackend*>(&backend);
    if (!litert_backend) {
        throw std::runtime_error(
            "LiteRTLMOrchestrator requires LiteRTLMBackend");
    }

    const auto* prepared =
        std::get_if<scheduler::LiteRTLMPreparedRequest>(&job.prepared);
    if (!prepared) {
        throw std::runtime_error(
            "LiteRTLMOrchestrator received a non-LiteRT-LM prepared request");
    }

    const std::string event_id = generateEventId();
    const bool streaming = prepared->generation.streaming;
    json profile =
        ModelConfigManager::getInstance().getChatTemplate(job.model_id);
    const std::string tool_call_delimiter =
        profile.value("tool_call_delimiter", "<|tool_call|>");

    std::string accumulated_text;
    std::string finish_reason = "stop";
    int completion_tokens = 0;
    bool inference_error = false;
    std::string error_message;

    if (streaming && job.callbacks.on_token) {
        StreamChunk role_chunk;
        role_chunk.id = job.session_id;
        role_chunk.model = job.model_id;
        role_chunk.role = "assistant";
        job.callbacks.on_token(role_chunk);
    }

    litert_backend->generateStructured(
        event_id,
        prepared->messages,
        prepared->tools,
        streaming,
        prepared->generation.max_tokens,
        prepared->generation.temperature,
        prepared->generation.top_p,
        prepared->generation.top_k,
        prepared->generation.presence_penalty,
        prepared->generation.frequency_penalty,
        [&](const IPCTokenEvent& token) {
            accumulated_text += token.content;
            ++completion_tokens;
            if (job.isCancelled()) {
                backend.terminateWorker(true);
                return;
            }
            if (!streaming || !job.callbacks.on_token ||
                accumulated_text.find(tool_call_delimiter) !=
                    std::string::npos) {
                return;
            }
            StreamChunk chunk;
            chunk.id = job.session_id;
            chunk.model = job.model_id;
            chunk.content_delta = token.content;
            job.callbacks.on_token(chunk);
        },
        [&](const IPCDoneEvent& done) {
            finish_reason = done.finish_reason.empty()
                ? "stop" : done.finish_reason;
        },
        [&](const IPCErrorEvent& error) {
            inference_error = true;
            error_message = error.message;
        });

    backend.resetKvAsync();
    if (inference_error) {
        throw GenAIException(
            GenAIErrorCode::INFERENCE_FAILED,
            "LiteRT-LM inference failed: " + error_message,
            500);
    }

    StandardResponse response;
    response.id = job.session_id;
    response.model = job.model_id;
    response.role = "assistant";
    response.finish_reason = finish_reason;
    response.prompt_tokens = estimateTokens(
        prepared->messages, prepared->tools);
    response.completion_tokens = completion_tokens;
    response.total_tokens = response.prompt_tokens + completion_tokens;

    const bool has_tool_call = !tool_call_delimiter.empty() &&
        accumulated_text.find(tool_call_delimiter) != std::string::npos;
    if (has_tool_call) {
        json tool_calls = parseToolCalls(
            accumulated_text, tool_call_delimiter);
        if (tool_calls.is_array() && !tool_calls.empty()) {
            response.tool_calls = std::move(tool_calls);
            response.finish_reason = "tool_calls";
            const std::string content = extractPreToolContent(
                accumulated_text, tool_call_delimiter);
            if (!content.empty()) {
                response.content = content;
            }
        } else {
            response.content = accumulated_text;
        }
    } else {
        response.content = accumulated_text;
    }

    if (streaming && job.callbacks.on_token) {
        StreamChunk finish_chunk;
        finish_chunk.id = job.session_id;
        finish_chunk.model = job.model_id;
        finish_chunk.finish_reason = response.finish_reason;
        job.callbacks.on_token(finish_chunk);
    }

    return response;
}

int LiteRTLMOrchestrator::estimateTokens(
    const json& messages,
    const json& tools) const {
    const std::size_t characters = messages.dump().size() + tools.dump().size();
    return static_cast<int>(characters / 4) + 1;
}

int LiteRTLMOrchestrator::applyContextEviction(
    json& messages,
    const json& tools,
    int max_context_length) const {
    if (max_context_length <= 0) {
        return 0;
    }

    const int limit = static_cast<int>(
        max_context_length * kCompactionThreshold);
    int evicted = 0;
    while (messages.size() > 2 && estimateTokens(messages, tools) > limit) {
        const bool has_system =
            messages.front().value("role", "") == "system";
        const std::size_t index = has_system ? 1 : 0;
        if (index >= messages.size() - 1) {
            break;
        }
        messages.erase(index);
        ++evicted;
    }
    return evicted;
}

json LiteRTLMOrchestrator::parseToolCalls(
    const std::string& text,
    const std::string& delimiter) const {
    const std::size_t delimiter_position = text.find(delimiter);
    if (delimiter_position == std::string::npos) {
        return nullptr;
    }

    std::string payload = text.substr(
        delimiter_position + delimiter.size());
    while (!payload.empty() &&
           std::isspace(static_cast<unsigned char>(payload.front()))) {
        payload.erase(payload.begin());
    }

    try {
        std::size_t start = payload.find('[');
        if (start == std::string::npos) {
            start = payload.find('{');
        }
        if (start == std::string::npos) {
            return nullptr;
        }

        std::string candidate = payload.substr(start);
        const char opening = candidate.front();
        const char closing = opening == '[' ? ']' : '}';
        int depth = 0;
        std::size_t end = std::string::npos;
        for (std::size_t index = 0; index < candidate.size(); ++index) {
            if (candidate[index] == opening) {
                ++depth;
            } else if (candidate[index] == closing && --depth == 0) {
                end = index + 1;
                break;
            }
        }
        if (end == std::string::npos) {
            return nullptr;
        }

        json parsed = json::parse(candidate.substr(0, end));
        if (parsed.is_object()) {
            parsed = json::array({parsed});
        }
        if (!parsed.is_array()) {
            return nullptr;
        }

        for (auto& tool_call : parsed) {
            if (!tool_call.contains("id")) {
                tool_call["id"] = generateEventId();
            }
            if (!tool_call.contains("type")) {
                tool_call["type"] = "function";
            }
        }
        return parsed;
    } catch (const std::exception& error) {
        LOG_WARN("[LiteRTLMOrchestrator] Tool call parsing failed: "
                 << error.what());
        return nullptr;
    }
}

std::string LiteRTLMOrchestrator::extractPreToolContent(
    const std::string& text,
    const std::string& delimiter) const {
    const std::size_t delimiter_position = text.find(delimiter);
    if (delimiter_position == std::string::npos) {
        return text;
    }
    std::string content = text.substr(0, delimiter_position);
    while (!content.empty() &&
           std::isspace(static_cast<unsigned char>(content.back()))) {
        content.pop_back();
    }
    return content;
}
