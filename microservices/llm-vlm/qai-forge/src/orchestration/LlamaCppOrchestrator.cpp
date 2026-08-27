// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifdef QAI_FORGE_BUILD_LLAMACPP

#include "qai_forge/orchestration/LlamaCppOrchestrator.h"
#include "qai_forge/backend/LlamaCppBackend.h"
#include "qai_forge/scheduler/InferenceJob.h"
#include "qai_forge/utils/Logger.h"
#include <sstream>
#include <stdexcept>
#include <random>

namespace qai_forge {

namespace {
// Generate a unique event ID for tracking requests
std::string generateEventId() {
    static std::mt19937_64 rng(std::random_device{}());
    std::ostringstream oss;
    oss << "evt-" << std::hex << rng();
    return oss.str();
}
} // anonymous namespace

StandardResponse LlamaCppOrchestrator::execute(
    const CreateChatCompletionRequest& request,
    const scheduler::SchedulerInvokeOptions& options,
    IGenerativeBackend& backend,
    OrchestratorStreamCallback callback,
    std::function<bool()> cancel) {

    // Cast to LlamaCppBackend to access HTTP methods
    auto* llama_backend = dynamic_cast<LlamaCppBackend*>(&backend);
    if (!llama_backend) {
        throw std::runtime_error("LlamaCppOrchestrator requires LlamaCppBackend");
    }

    // Merge response_history into request.messages
    static const json kEmptyResponseHistory = json::array();
    const json& response_history = options.use_response_history
        ? options.response_history : kEmptyResponseHistory;
    json merged_messages = json::array();
    if (response_history.is_array()) {
        for (const auto& msg : response_history) {
            merged_messages.push_back(msg);
        }
    }
    if (request.messages.is_array()) {
        for (const auto& msg : request.messages) {
            merged_messages.push_back(msg);
        }
    }

    // Build OpenAI chat completions request
    json chat_request = buildChatCompletionsRequest(request, merged_messages);

    // Determine streaming mode
    bool streaming = callback != nullptr;
    chat_request["stream"] = streaming;

    // Request token usage in streaming responses
    if (streaming) {
        chat_request["stream_options"] = {{"include_usage", true}};
    }

    // Generate event ID for this request
    std::string event_id = generateEventId();

    // Accumulation variables for streaming
    std::string accumulated_content;
    json accumulated_tool_calls = json::array();
    std::string finish_reason;
    int prompt_tokens = 0;
    int completion_tokens = 0;

    if (streaming) {
        // Streaming mode: use httpPostStreaming
        llama_backend->httpPostStreaming("/v1/chat/completions", chat_request,
            [&](const std::string& sse_chunk) {
                // Check cancellation
                if (cancel && cancel()) {
                    return;
                }

                try {
                    // Parse SSE chunk and invoke callback
                    handleSseChunk(sse_chunk, callback, event_id, request.model,
                                 accumulated_content, accumulated_tool_calls,
                                 finish_reason, prompt_tokens, completion_tokens);
                } catch (const std::exception& e) {
                    LOG_ERROR("[LlamaCppOrchestrator] handleSseChunk threw: "
                              + std::string(e.what())
                              + " chunk=" + sse_chunk.substr(0, 128));
                } catch (...) {
                    LOG_ERROR("[LlamaCppOrchestrator] handleSseChunk threw unknown exception"
                              " chunk=" + sse_chunk.substr(0, 128));
                }
            });
    } else {
        // Blocking mode: use httpPostBlocking
        json response = llama_backend->httpPostBlocking("/v1/chat/completions", chat_request);

        // Extract content from response
        if (response.contains("choices") && response["choices"].is_array() && !response["choices"].empty()) {
            const auto& choice = response["choices"][0];
            if (choice.contains("message")) {
                const auto& message = choice["message"];
                if (message.contains("content") && message["content"].is_string()) {
                    accumulated_content = message["content"].get<std::string>();
                }
                if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
                    accumulated_tool_calls = message["tool_calls"];
                }
            }
            if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
                finish_reason = choice["finish_reason"].get<std::string>();
            }
        }

        // Extract token usage
        if (response.contains("usage") && response["usage"].is_object()) {
            const auto& usage = response["usage"];
            if (usage.contains("prompt_tokens") && usage["prompt_tokens"].is_number()) {
                prompt_tokens = usage["prompt_tokens"].get<int>();
            }
            if (usage.contains("completion_tokens") && usage["completion_tokens"].is_number()) {
                completion_tokens = usage["completion_tokens"].get<int>();
            }
        }
    }

    // Build and return StandardResponse
    return buildStandardResponse(event_id, request.model,
                                accumulated_content, accumulated_tool_calls,
                                finish_reason, prompt_tokens, completion_tokens);
}

json LlamaCppOrchestrator::buildChatCompletionsRequest(
    const CreateChatCompletionRequest& request,
    const json& merged_messages) {

    json chat_request = json::object();
    chat_request["model"] = request.model;
    chat_request["messages"] = merged_messages;

    // Add generation parameters
    int max_tokens = request.max_completion_tokens.value_or(0);
    if (max_tokens > 0) {
        chat_request["max_tokens"] = max_tokens;
    }
    if (request.temperature.has_value()) {
        chat_request["temperature"] = request.temperature.value();
    }
    if (request.top_p.has_value()) {
        chat_request["top_p"] = request.top_p.value();
    }
    if (request.presence_penalty.has_value()) {
        chat_request["presence_penalty"] = request.presence_penalty.value();
    }
    if (request.frequency_penalty.has_value()) {
        chat_request["frequency_penalty"] = request.frequency_penalty.value();
    }

    // Add tools if present
    if (request.tools.has_value() && request.tools.value().is_array() && !request.tools.value().empty()) {
        chat_request["tools"] = request.tools.value();
    }

    // Disable Qwen3 thinking when reasoning effort is "none"
    if (request.reasoning_effort.has_value() && request.reasoning_effort.value() == "none") {
        chat_request["chat_template_kwargs"] = {{"enable_thinking", false}};
    }

    return chat_request;
}

void LlamaCppOrchestrator::handleSseChunk(
    const std::string& sse_chunk,
    OrchestratorStreamCallback callback,
    const std::string& event_id,
    const std::string& model,
    std::string& accumulated_content,
    json& accumulated_tool_calls,
    std::string& finish_reason,
    int& prompt_tokens,
    int& completion_tokens) {

    // Parse SSE format: "data: {...}\n\n"
    if (sse_chunk.find("data: [DONE]") != std::string::npos) {
        return;
    }

    size_t data_pos = sse_chunk.find("data: ");
    if (data_pos == std::string::npos) {
        return;
    }

    std::string json_str = sse_chunk.substr(data_pos + 6);
    // Remove trailing whitespace
    while (!json_str.empty() && (json_str.back() == '\n' || json_str.back() == '\r')) {
        json_str.pop_back();
    }

    if (json_str.empty()) {
        return;
    }

    try {
        json chunk_data = json::parse(json_str);

        // Extract delta from choices[0].delta
        if (chunk_data.contains("choices") && chunk_data["choices"].is_array() && !chunk_data["choices"].empty()) {
            const auto& choice = chunk_data["choices"][0];

            if (choice.contains("delta") && choice["delta"].is_object()) {
                const auto& delta = choice["delta"];

                // Accumulate content
                if (delta.contains("content") && delta["content"].is_string()) {
                    std::string content_delta = delta["content"].get<std::string>();
                    accumulated_content += content_delta;

                    // Invoke callback with content chunk
                    StreamChunk stream_chunk;
                    stream_chunk.id = event_id;
                    stream_chunk.model = model;
                    stream_chunk.content_delta = content_delta;
                    stream_chunk.finish_reason = std::nullopt;
                    callback(stream_chunk);
                }

                // Accumulate tool calls and stream them to the client
                if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                    for (const auto& tool_call_delta : delta["tool_calls"]) {
                        // Merge tool call deltas (simplified - assumes index-based merging)
                        if (tool_call_delta.contains("index") && tool_call_delta["index"].is_number()) {
                            int index = tool_call_delta["index"].get<int>();

                            // Ensure array is large enough
                            while (accumulated_tool_calls.size() <= static_cast<size_t>(index)) {
                                accumulated_tool_calls.push_back(json::object());
                            }

                            // Merge fields
                            if (tool_call_delta.contains("id")) {
                                accumulated_tool_calls[index]["id"] = tool_call_delta["id"];
                            }
                            if (tool_call_delta.contains("type")) {
                                accumulated_tool_calls[index]["type"] = tool_call_delta["type"];
                            }
                            if (tool_call_delta.contains("function")) {
                                if (!accumulated_tool_calls[index].contains("function")) {
                                    accumulated_tool_calls[index]["function"] = json::object();
                                }
                                const auto& func_delta = tool_call_delta["function"];
                                if (func_delta.contains("name")) {
                                    accumulated_tool_calls[index]["function"]["name"] = func_delta["name"];
                                }
                                if (func_delta.contains("arguments")) {
                                    std::string args_delta = func_delta["arguments"].get<std::string>();
                                    if (!accumulated_tool_calls[index]["function"].contains("arguments")) {
                                        accumulated_tool_calls[index]["function"]["arguments"] = "";
                                    }
                                    accumulated_tool_calls[index]["function"]["arguments"] =
                                        accumulated_tool_calls[index]["function"]["arguments"].get<std::string>() + args_delta;
                                }
                            }
                        }
                    }

                    // Stream the tool_calls delta to the client so Hermes/clients
                    // receive the tool call payload in the streaming response
                    StreamChunk tool_chunk;
                    tool_chunk.id = event_id;
                    tool_chunk.model = model;
                    tool_chunk.tool_calls = delta["tool_calls"];
                    callback(tool_chunk);
                }
            }

            // Extract finish_reason
            if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                finish_reason = choice["finish_reason"].get<std::string>();
            }
        }

        // Extract token usage (if present in streaming)
        if (chunk_data.contains("usage") && chunk_data["usage"].is_object()) {
            const auto& usage = chunk_data["usage"];
            if (usage.contains("prompt_tokens") && usage["prompt_tokens"].is_number()) {
                prompt_tokens = usage["prompt_tokens"].get<int>();
            }
            if (usage.contains("completion_tokens") && usage["completion_tokens"].is_number()) {
                completion_tokens = usage["completion_tokens"].get<int>();
            }
        }

    } catch (const json::exception& e) {
        LOG_ERROR("[LlamaCppOrchestrator] Failed to parse SSE chunk: " + std::string(e.what()));
    }
}

StandardResponse LlamaCppOrchestrator::buildStandardResponse(
    const std::string& event_id,
    const std::string& model,
    const std::string& accumulated_content,
    const json& accumulated_tool_calls,
    const std::string& finish_reason,
    int prompt_tokens,
    int completion_tokens) {

    StandardResponse response;
    response.id = event_id;
    response.model = model;
    response.content = accumulated_content;
    response.finish_reason = finish_reason.empty() ? "stop" : finish_reason;

    // Add tool calls if present
    if (accumulated_tool_calls.is_array() && !accumulated_tool_calls.empty()) {
        response.tool_calls = accumulated_tool_calls;
    }

    // Add usage information
    response.prompt_tokens = prompt_tokens;
    response.completion_tokens = completion_tokens;
    response.total_tokens = prompt_tokens + completion_tokens;
    response.reasoning_tokens = 0;

    return response;
}

} // namespace qai_forge

#endif // QAI_FORGE_BUILD_LLAMACPP
