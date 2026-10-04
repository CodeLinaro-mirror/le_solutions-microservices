// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/QaiForge.h"

#include <string>

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

struct GenerativeJobContext {
    std::string job_id;
    std::string model_id;
    CreateChatCompletionRequest request;
    qai_forge::GenerateOptions caller;
    std::string execution_session_id;

    std::string tool_chain_id;
    JobKind kind = JobKind::HTTP_NON_STREAMING;
    JobPriority priority = JobPriority::ANY_REQUEST;
    bool tool_continuation = false;
    bool skip_post_turn_summarization = false;
};

} // namespace scheduler
