// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/scheduler/GenerativeJob.h"
#include "qai_forge/scheduler/SubmitResult.h"

#include <cstddef>
#include <string>
#include <unordered_map>

namespace scheduler {

struct SchedulerInvokeOptions {
    std::string response_id;
    std::string previous_response_id;
    std::string session_id;
    JobKind kind = JobKind::HTTP_NON_STREAMING;
    JobPriority priority = JobPriority::NEW_REQUEST;
    bool tool_output_submission = false;
    bool allow_tool_chain_fallback = false;
    bool skip_summarization_middleware = false;
    bool use_response_history = false;
    json response_history = json::array();

    std::string summary_content;
    int summary_token_count = 0;
    std::unordered_map<std::string, std::string> facts;
    std::size_t evicted_message_count = 0;
};

using InferenceJob = GenerativeJob;
using InferenceJobPtr = GenerativeJobPtr;

} // namespace scheduler
