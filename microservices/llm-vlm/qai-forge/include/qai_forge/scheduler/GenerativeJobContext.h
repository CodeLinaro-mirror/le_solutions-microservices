// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/QaiForge.h"
#include "qai_forge/utils/ImageUtils.h"

#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace scheduler {

enum class JobPriority {
    CONTROL = 0,
    TOOL_CONTINUATION = 1,
    ANY_REQUEST = 2,
};

enum class JobKind {
    HTTP_NON_STREAMING,
    HTTP_STREAMING,
    WEBSOCKET,
    MCP_ROUND,
    INTERNAL_SUMMARIZATION,
};

struct GenerationConfig {
    int max_tokens = 512;
    float temperature = 1.0f;
    float top_p = 1.0f;
    int top_k = 40;
    float presence_penalty = 0.0f;
    float frequency_penalty = 0.0f;
    bool streaming = false;
    bool use_reasoning = false;
    int thinking_budget = 0;
    std::string thinking_start_tag = "<think>";
    std::string thinking_end_tag = "</think>";
};

struct PreparedVisionInputs {
    std::vector<std::string> paths;
    std::shared_ptr<ImageUtils::TempFileGuard> lifetime;
};

struct GeniePreparedRequest {
    std::string final_prompt;
    GenerationConfig generation;
    PreparedVisionInputs vision;
    json tools = json::array();
    json conversation_messages = json::array();
    ConversationMemoryUpdate input_memory;
};

struct LlamaCppPreparedRequest {
    json chat_completions_body;
};

struct LiteRTLMPreparedRequest {
    json messages = json::array();
    json tools = json::array();
    GenerationConfig generation;
};

using PreparedGenerativeRequest =
    std::variant<GeniePreparedRequest,
                 LlamaCppPreparedRequest,
                 LiteRTLMPreparedRequest>;

struct GenerativeJobContext {
    std::string job_id;
    std::string model_id;
    CreateChatCompletionRequest request;
    qai_forge::GenerateOptions caller;

    std::string tool_chain_id;
    std::string conversation_memory_write_key;
    JobKind kind = JobKind::HTTP_NON_STREAMING;
    JobPriority priority = JobPriority::ANY_REQUEST;
    bool tool_continuation = false;
    bool skip_post_turn_summarization = false;
};

} // namespace scheduler
