// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// genai-engine.hpp — Shared types for LlmEngine and VlmEngine (Layer 4)
//
// Replaces llm-buffer.h. No fixed-size char arrays, no OpenAI-shaped structs.
// Layer 4 only knows about strings, byte buffers, and generation parameters.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once
#include <string>
#include <functional>
#include <cstdint>
#include <cstddef>

// ── Generation parameters ─────────────────────────────────────────────────────
struct GenerationConfig {
    int   max_tokens         = 1024;
    float temperature        = 1.0f;
    float top_p              = 1.0f;
    int   top_k              = 40;
    float presence_penalty   = 0.0f;
    float frequency_penalty  = 0.0f;

    // When true, <think>...</think> tokens are passed through raw to the caller.
    // When false (default), think blocks are silently filtered inside LlmEngine.
    // Layer 2's ReasoningRouter handles routing when this is true.
    bool  bypass_think_filter = false;
};

// ── Image buffer ──────────────────────────────────────────────────────────────
// Caller owns the memory; must remain valid for the duration of generate().
struct ImageBuffer {
    const uint8_t* data = nullptr;
    size_t         size = 0;
};

// ── Token callback ────────────────────────────────────────────────────────────
// Called once per token emitted by the model.
//   token         : the token string (may be empty on the final call)
//   finish_reason : empty string for intermediate tokens;
//                   "stop" or "length" on the final call
//
// genai-lib always streams — it never accumulates tokens internally.
// Layer 3 (worker subprocess) is responsible for accumulation when the
// upstream request is non-streaming.
using TokenCallback = std::function<void(const std::string& token,
                                          const std::string& finish_reason)>;
