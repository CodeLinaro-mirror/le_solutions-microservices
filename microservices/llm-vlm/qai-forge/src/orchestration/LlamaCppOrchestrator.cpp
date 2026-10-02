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

namespace {
std::string generateEventId() {
    static std::mt19937_64 rng(std::random_device{}());
    std::ostringstream oss;
    oss << "evt-" << std::hex << rng();
    return oss.str();
}
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// LlamaCppOrchestrator::PreparedState — concrete opaque prepared-state
// subclass. Holds everything that used to live on
// scheduler::LlamaCppPreparedRequest.
// ─────────────────────────────────────────────────────────────────────────────
struct LlamaCppOrchestrator::PreparedState : qai_forge::PreparedGenerationState {
    json messages = json::array();
    json tools = json::array();
    std::vector<std::vector<uint8_t>> vision_buffers;

    int max_tokens = 512;
    float temperature = 0.7f;
    float top_p = 0.9f;
    int top_k = 40;
    float presence_penalty = 0.0f;
    float frequency_penalty = 0.0f;
};

// ─────────────────────────────────────────────────────────────────────────────
// prepare()
//
// Ports the body of the old createJob() — merges inline images extracted
// from message content, applies front eviction, and stashes generation
// parameters into a PreparedState.
// ─────────────────────────────────────────────────────────────────────────────
std::unique_ptr<qai_forge::PreparedGenerationState> LlamaCppOrchestrator::prepare(
    const CreateChatCompletionRequest& request_in,
    const qai_forge::ConversationMemorySnapshot& /*memory*/,
    const std::string& /*session_id*/) const {
    json merged_messages = request_in.messages;
    if (!merged_messages.is_array()) {
        merged_messages = json::array();
    }

    std::vector<std::vector<uint8_t>> inline_images =
        extractImagesFromMessages(merged_messages);

    json tools = request_in.tools.value_or(json::array());
    const int evicted = qai_forge::utils::applyFrontEviction(
        merged_messages,
        tools,
        ModelConfigManager::getInstance().getContextSize(request_in.model),
        kCompactionThreshold);
    if (evicted > 0) {
        LOG_INFO("[LlamaCppOrchestrator] Evicted " << evicted
                 << " messages while preparing model=" << request_in.model);
    }

    auto prepared = std::make_unique<PreparedState>();
    prepared->messages = std::move(merged_messages);
    prepared->tools = std::move(tools);
    prepared->vision_buffers = request_in.raw_images;
    if (!inline_images.empty()) {
        prepared->vision_buffers.insert(
            prepared->vision_buffers.end(),
            std::make_move_iterator(inline_images.begin()),
            std::make_move_iterator(inline_images.end()));
    }
    prepared->max_tokens = request_in.max_completion_tokens.value_or(512);
    prepared->temperature = request_in.temperature.value_or(0.7f);
    prepared->top_p = request_in.top_p.value_or(0.9f);
    prepared->top_k = request_in.top_k.value_or(40);
    prepared->presence_penalty = request_in.presence_penalty.value_or(0.0f);
    prepared->frequency_penalty = request_in.frequency_penalty.value_or(0.0f);

    return prepared;
}

// ─────────────────────────────────────────────────────────────────────────────
// execute()
//
// Ports the body of the old execute() nearly verbatim — job.*/callbacks.on_token
// references become info.*/on_token parameter references.
// ─────────────────────────────────────────────────────────────────────────────
StandardResponse LlamaCppOrchestrator::execute(
    const qai_forge::GenerationJobInfo& info,
    qai_forge::PreparedGenerationState& prepared_base,
    qai_forge::IGenerativeBackend& backend,
    std::function<void(const StreamChunk&)> on_token) const {
    auto* llama_backend = dynamic_cast<LlamaCppBackend*>(&backend);
    if (!llama_backend) {
        throw std::runtime_error(
            "LlamaCppOrchestrator requires LlamaCppBackend");
    }

    auto* prepared = dynamic_cast<PreparedState*>(&prepared_base);
    if (!prepared) {
        throw std::runtime_error(
            "LlamaCppOrchestrator received a prepared state it did not create");
    }

    const bool streaming = info.streaming;
    json profile =
        ModelConfigManager::getInstance().getChatTemplate(info.model_id);
    const std::string tool_call_delimiter =
        profile.value("tool_call_delimiter", "<|tool_call|>");

    std::string accumulated_text;
    std::string finish_reason = "stop";
    int completion_tokens = 0;
    bool inference_error = false;
    std::string error_message;

    if (streaming && on_token) {
        StreamChunk role_chunk;
        role_chunk.id = info.session_id;
        role_chunk.model = info.model_id;
        role_chunk.role = "assistant";
        on_token(role_chunk);
    }

    llama_backend->generateStructured(
        info.response_id,
        info.session_id,
        prepared->messages,
        prepared->tools,
        streaming,
        prepared->max_tokens,
        prepared->temperature,
        prepared->top_p,
        prepared->top_k,
        prepared->presence_penalty,
        prepared->frequency_penalty,
        [&](const IPCTokenEvent& token) {
            accumulated_text += token.content;
            ++completion_tokens;
            if (!streaming || !on_token ||
                accumulated_text.find(tool_call_delimiter) !=
                    std::string::npos) {
                return;
            }
            StreamChunk chunk;
            chunk.id = info.session_id;
            chunk.model = info.model_id;
            chunk.content_delta = token.content;
            on_token(chunk);
        },
        [&](const IPCDoneEvent& done) {
            finish_reason = done.finish_reason.empty()
                ? "stop" : done.finish_reason;
        },
        [&](const IPCErrorEvent& error) {
            inference_error = true;
            error_message = error.message;
        },
        prepared->vision_buffers);

    backend.resetKvAsync();
    if (inference_error) {
        throw GenAIException(
            GenAIErrorCode::INFERENCE_FAILED,
            "llama.cpp inference failed: " + error_message,
            500);
    }

    StandardResponse response;
    response.id = info.session_id;
    response.model = info.model_id;
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

    if (streaming && on_token) {
        StreamChunk finish_chunk;
        finish_chunk.id = info.session_id;
        finish_chunk.model = info.model_id;
        finish_chunk.finish_reason = response.finish_reason;
        on_token(finish_chunk);
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

#endif // QAI_FORGE_BUILD_LLAMACPP
