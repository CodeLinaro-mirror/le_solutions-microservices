// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/QaiForge.h"
#include "qai_forge/scheduler/ConversationMemoryCoordinator.h"

#include <cstdint>
#include <memory>
#include <optional>
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

// GenerationConfig is retained here for backward-source-compatibility with
// any code that still references scheduler::GenerationConfig — the
// individual orchestrator plugins now define their own private prepared-
// state structs (see qai_forge::PreparedGenerationState) that embed
// equivalent generation-parameter fields directly, since the old closed
// std::variant<GeniePreparedRequest, LlamaCppPreparedRequest,
// LiteRTLMPreparedRequest> has been removed in favor of the public,
// per-orchestrator-opaque IGenerativeOrchestrator::prepare() contract.
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
    std::vector<std::vector<uint8_t>> buffers;
};

struct GenerativeJobContext {
    std::string job_id;
    std::string model_id;
    CreateChatCompletionRequest request;
    qai_forge::GenerateOptions caller;
    std::string execution_session_id;

    std::string tool_chain_id;
    std::optional<MemoryTurnCommitToken> memory_turn;
    std::optional<GenieMemoryState> memory_state;
    JobKind kind = JobKind::HTTP_NON_STREAMING;
    JobPriority priority = JobPriority::ANY_REQUEST;
    bool tool_continuation = false;
    bool skip_post_turn_summarization = false;
};

} // namespace scheduler
