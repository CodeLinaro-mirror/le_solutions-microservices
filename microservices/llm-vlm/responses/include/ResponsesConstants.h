// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

namespace ResponsesConstants {

constexpr const char* DEFAULT_SYSTEM_PROMPT = "You are a helpful assistant.";

constexpr double TOKENS_PER_WORD = 1.3;
constexpr int IMAGE_TOKEN_COST = 765;
constexpr int MESSAGE_OVERHEAD_TOKENS = 4;
constexpr int MAX_COMPLETION_SAFETY_MARGIN = 64;
constexpr int MIN_USEFUL_COMPLETION_TOKENS = 64;
constexpr double SUMMARIZATION_CONTEXT_THRESHOLD = 0.9;
constexpr double SUMMARIZATION_SUMMARY_SIZE_RATIO = 0.2;
constexpr double SUMMARIZATION_SYSTEM_PROMPT_OVERHEAD = 1.3;
constexpr double SUMMARIZATION_MAX_COMPLETION_MULTIPLIER = 0.5;

constexpr const char* ERROR_CODE_CONTEXT_LENGTH_EXCEEDED =
    "context_length_exceeded";

constexpr const char* PROMPT_TOO_LONG =
    "Prompt consumes nearly the entire {context_size}-token context window. "
    "Only {cap} tokens remain for the response, which is below the minimum "
    "useful size. Please shorten your prompt.";

constexpr const char* PROMPT_TOO_LONG_TOOL_RESPONSE =
    "The combined prompt and tool response consume nearly the entire "
    "{context_size}-token context window. Only {cap} tokens remain for the "
    "response, which is below the minimum useful size. Please shorten your "
    "prompt or return a smaller tool response.";

constexpr const char* CONTEXT_LENGTH_EXCEEDED =
    "max_output_tokens={requested} exceeds the available budget for this "
    "model. Reduce max_output_tokens to {cap} or lower, or shorten your "
    "prompt. Model context window: {context_size}.";

constexpr const char* CONTEXT_LENGTH_EXCEEDED_TOOL_RESPONSE =
    "The combined prompt and tool response exceed the available budget for "
    "this model. Reduce max_output_tokens to {cap} or lower, shorten your "
    "prompt, or return a smaller tool response. Model context window: "
    "{context_size}.";

} // namespace ResponsesConstants
