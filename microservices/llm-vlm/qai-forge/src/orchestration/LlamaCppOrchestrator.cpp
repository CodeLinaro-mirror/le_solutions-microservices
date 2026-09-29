// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifdef QAI_FORGE_BUILD_LLAMACPP

#include "qai_forge/orchestration/LlamaCppOrchestrator.h"
#include "qai_forge/backend/LlamaCppBackend.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/ImageUtils.h"
#include "qai_forge/utils/Logger.h"
#include "qai_forge/utils/TokenEstimation.h"

#include <cctype>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

namespace qai_forge {

namespace {
std::string generateEventId() {
    static std::mt19937_64 rng(std::random_device{}());
    std::ostringstream oss;
    oss << "evt-" << std::hex << rng();
    return oss.str();
}
} // namespace

scheduler::GenerativeJobPtr LlamaCppOrchestrator::createJob(
    scheduler::GenerativeJobContext context,
    scheduler::GenerativeCallbacks callbacks) const {
    json merged_messages = std::move(context.request.messages);
    if (!merged_messages.is_array()) {
        merged_messages = json::array();
    }

    std::vector<std::vector<uint8_t>> inline_images =
        extractImagesFromMessages(merged_messages);

    json tools = std::move(context.request.tools).value_or(json::array());
    const int evicted = qai_forge::utils::applyFrontEviction(
        merged_messages,
        tools,
        ModelConfigManager::getInstance().getContextSize(context.model_id),
        kCompactionThreshold);
    if (evicted > 0) {
        LOG_INFO("[LlamaCppOrchestrator] Evicted " << evicted
                 << " messages while preparing model=" << context.model_id);
    }

    scheduler::LlamaCppPreparedRequest prepared;
    prepared.messages = std::move(merged_messages);
    prepared.tools = std::move(tools);
    prepared.vision.buffers = std::move(context.request.raw_images);
    if (!inline_images.empty()) {
        prepared.vision.buffers.insert(
            prepared.vision.buffers.end(),
            std::make_move_iterator(inline_images.begin()),
            std::make_move_iterator(inline_images.end()));
    }
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
    job->session_id = context.execution_session_id.empty()
        ? context.job_id : std::move(context.execution_session_id);
    job->job_id = std::move(context.job_id);
    job->model_id = std::move(context.model_id);
    job->tool_chain_id = std::move(context.tool_chain_id);
    job->memory_turn = context.memory_turn;
    job->kind = context.kind;
    job->priority = context.priority;
    job->prepared = std::move(prepared);
    job->skip_post_turn_summarization =
        context.skip_post_turn_summarization;
    job->callbacks = std::move(callbacks);
    return job;
}

StandardResponse LlamaCppOrchestrator::execute(
    scheduler::GenerativeJob& job,
    IGenerativeBackend& backend) const {
    auto* llama_backend = dynamic_cast<LlamaCppBackend*>(&backend);
    if (!llama_backend) {
        throw std::runtime_error(
            "LlamaCppOrchestrator requires LlamaCppBackend");
    }

    const auto* prepared =
        std::get_if<scheduler::LlamaCppPreparedRequest>(&job.prepared);
    if (!prepared) {
        throw std::runtime_error(
            "LlamaCppOrchestrator received a non-llama.cpp prepared request");
    }

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

    llama_backend->generateStructured(
        job.response_id,
        job.session_id,
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
        },
        prepared->vision.buffers);

    backend.resetKvAsync();
    if (inference_error) {
        throw GenAIException(
            GenAIErrorCode::INFERENCE_FAILED,
            "llama.cpp inference failed: " + error_message,
            500);
    }

    StandardResponse response;
    response.id = job.session_id;
    response.model = job.model_id;
    response.role = "assistant";
    response.finish_reason = finish_reason;
    response.prompt_tokens = qai_forge::utils::estimateTokensApprox(
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

std::vector<std::vector<uint8_t>> LlamaCppOrchestrator::extractImagesFromMessages(
    json& messages) const {
    std::vector<std::vector<uint8_t>> images;
    if (!messages.is_array()) {
        return images;
    }

    for (auto& message : messages) {
        if (!message.is_object()) {
            continue;
        }
        auto content_it = message.find("content");
        if (content_it == message.end() || !content_it->is_array()) {
            continue;
        }

        std::string combined_text;
        bool has_image = false;
        for (auto& part : *content_it) {
            if (!part.is_object()) {
                continue;
            }
            const std::string type = part.value("type", "");
            if (type != "image_url" && type != "input_image") {
                if (type == "text") {
                    if (!combined_text.empty()) {
                        combined_text += "\n";
                    }
                    combined_text += part.value("text", "");
                }
                continue;
            }

            has_image = true;
            auto image_url = part.value("image_url", json::object());
            const std::string url = image_url.is_string()
                ? image_url.get<std::string>()
                : image_url.value("url", "");
            if (url.empty()) {
                continue;
            }
            try {
                ImageData image_data = ImageUtils::resolveImage(url);
                images.push_back(ImageUtils::base64Decode(image_data.base64_data));
            } catch (const std::exception& error) {
                LOG_WARN("[LlamaCppOrchestrator] Failed to resolve inline image: "
                         << error.what());
            }
        }

        if (has_image) {
            *content_it = combined_text;
        }
    }
    return images;
}

json LlamaCppOrchestrator::parseToolCalls(
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
        LOG_WARN("[LlamaCppOrchestrator] Tool call parsing failed: "
                 << error.what());
        return nullptr;
    }
}

std::string LlamaCppOrchestrator::extractPreToolContent(
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

} // namespace qai_forge

#endif // QAI_FORGE_BUILD_LLAMACPP
