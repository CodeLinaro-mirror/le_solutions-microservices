// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/adapters/ModelAdapter.h"

// ─────────────────────────────────────────────────────────────────────────────
// Qwen3Adapter — Qwen 3 / Qwen 3-VL Model Family
//
// Handles model-specific transformations for the Qwen 3 family:
//   - Vision: <|image|> token format (different from Qwen 2.5)
//   - Tools:  Native Qwen3 tool call format with <tool_call> tags
//             but different JSON structure than Qwen 2.5
//   - Thinking: Supports <think>...</think> reasoning blocks
//               (bypass_think_filter=true is set for this adapter)
// ─────────────────────────────────────────────────────────────────────────────
class Qwen3Adapter : public ModelAdapter {
public:
    // ── Vision Preprocessing ───────────────────────────────────────────────────
    json preprocessVision(const json& messages) const override;

    // ── Tool Calling ───────────────────────────────────────────────────────────
    std::string formatToolInstructions(const json& tools) const override;
    json parseToolCalls(const std::string& response_text) const override;
    std::string formatToolResponse(const json& tool_results) const override;

    // ── System Prompt ──────────────────────────────────────────────────────────
    std::string buildSystemPrompt(const json& chat_template,
                                  const std::string& user_system,
                                  const json& tools) const override;

    // ── Identity ───────────────────────────────────────────────────────────────
    std::string adapterName() const override { return "Qwen3Adapter"; }
    bool supportsVision() const override { return true; }
    bool supportsToolCalling() const override { return true; }
};
