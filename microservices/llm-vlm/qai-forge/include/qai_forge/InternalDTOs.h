// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <string>
#include <optional>
#include <vector>
#include <map>
#include <nlohmann/json.hpp>

using json = nlohmann::ordered_json;

// ─────────────────────────────────────────────────────────────────────────────
// Transport-Agnostic Response DTOs (Layer 2 → Layer 1 boundary)
//
// Layer 2 (orchestration) yields these objects. Layer 1 (Drogon HTTP) is
// responsible for formatting them into the OpenAI HTTP/SSE wire format.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Base fields shared by all response chunks.
 */
struct BaseResponseChunk {
    std::string id;
    std::string model;
};

/**
 * A single streaming token delta.
 * Yielded by the InferenceExecutor → ReasoningRouter pipeline.
 */
struct StreamChunk : BaseResponseChunk {
    std::optional<std::string> role;
    std::optional<std::string> content_delta;
    std::optional<std::string> reasoning_content; // For reasoning models (e.g. DeepSeek-R1)
    std::optional<std::string> finish_reason;
};

/**
 * A streaming tool call chunk.
 */
struct ToolCallChunk : BaseResponseChunk {
    json tool_calls; // Array of tool call objects
    std::optional<std::string> finish_reason;
};

/**
 * A complete non-streaming response.
 */
struct StandardResponse : BaseResponseChunk {
    std::string role;
    std::optional<std::string> content;
    std::optional<std::string> reasoning_content;
    std::optional<json> tool_calls;
    std::string finish_reason;
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total_tokens = 0;
    int reasoning_tokens = 0;  // Thinking tokens generated (for output_tokens_details)
};

// ─────────────────────────────────────────────────────────────────────────────
// Domain Exceptions (Layer 2 raises these; Layer 1 maps them to HTTP errors)
// ─────────────────────────────────────────────────────────────────────────────

enum class GenAIErrorCode {
    MODEL_NOT_FOUND,
    CONTEXT_LENGTH_EXCEEDED,
    TOOL_RESPONSE_TIMEOUT,
    INFERENCE_FAILED,
    INSUFFICIENT_MEMORY,
    HARDWARE_UNAVAILABLE,
    INVALID_REQUEST,
    INTERNAL_ERROR
};

struct GenAIException : std::exception {
    GenAIErrorCode code;
    std::string message;
    int http_status;

    GenAIException(GenAIErrorCode code, std::string message, int http_status)
        : code(code), message(std::move(message)), http_status(http_status) {}

    const char* what() const noexcept override {
        return message.c_str();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Request DTO (parsed from HTTP body by Layer 1, passed to Layer 2)
// ─────────────────────────────────────────────────────────────────────────────

struct CreateChatCompletionRequest {
    std::string model;
    json messages;                              // Raw JSON array of messages
    bool stream = false;
    std::optional<int> max_completion_tokens;
    std::optional<float> temperature;
    std::optional<float> top_p;
    std::optional<int> top_k;
    std::optional<float> frequency_penalty;
    std::optional<float> presence_penalty;
    std::optional<json> tools;
    std::optional<std::string> user;

    // ── Reasoning model parameters ────────────────────────────────────────────
    // reasoning_effort: "none"/"minimal"/"low"/"medium"/"high"/"xhigh"
    // Controls the thinking budget via ReasoningBudgetCalculator.
    // When absent, defaults to "medium" for models that support thinking.
    std::optional<std::string> reasoning_effort;

    // reasoning_summary: "" / "auto" / "concise" / "detailed"
    // When set, a "reasoning" output item is included in the Responses API output[].
    std::optional<std::string> reasoning_summary;

    // ── OIP raw prompt mode ───────────────────────────────────────────────────
    // Set by InferController when the OIP /generate request uses "text_input"
    // (raw pre-formatted prompt) instead of "messages" (server applies template).
    // When set, ChatOrchestratorImpl::buildContextPrompt() returns this string
    // directly without applying the chat template.
    std::optional<std::string> raw_prompt;

    static CreateChatCompletionRequest from_json(const json& j) {
        CreateChatCompletionRequest req;
        req.model = j.at("model").get<std::string>();
        req.messages = j.at("messages");
        req.stream = j.value("stream", false);
        if (j.contains("max_completion_tokens") && !j["max_completion_tokens"].is_null())
            req.max_completion_tokens = j["max_completion_tokens"].get<int>();
        if (j.contains("temperature") && !j["temperature"].is_null())
            req.temperature = j["temperature"].get<float>();
        if (j.contains("top_p") && !j["top_p"].is_null())
            req.top_p = j["top_p"].get<float>();
        if (j.contains("top_k") && !j["top_k"].is_null())
            req.top_k = j["top_k"].get<int>();
        if (j.contains("frequency_penalty") && !j["frequency_penalty"].is_null())
            req.frequency_penalty = j["frequency_penalty"].get<float>();
        if (j.contains("presence_penalty") && !j["presence_penalty"].is_null())
            req.presence_penalty = j["presence_penalty"].get<float>();
        if (j.contains("tools") && !j["tools"].is_null())
            req.tools = j["tools"];
        if (j.contains("user") && !j["user"].is_null())
            req.user = j["user"].get<std::string>();
        if (j.contains("reasoning_effort") && !j["reasoning_effort"].is_null())
            req.reasoning_effort = j["reasoning_effort"].get<std::string>();
        if (j.contains("reasoning_summary") && !j["reasoning_summary"].is_null())
            req.reasoning_summary = j["reasoning_summary"].get<std::string>();
        return req;
    }
};
