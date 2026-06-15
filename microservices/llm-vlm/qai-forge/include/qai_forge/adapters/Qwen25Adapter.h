// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/adapters/ModelAdapter.h"

// ─────────────────────────────────────────────────────────────────────────────
// Qwen25Adapter — Qwen 2.5 / Qwen 2.5-VL Model Family
//
// Handles model-specific transformations for the Qwen 2.5 family:
//   - Vision: <|vision_start|><|image_pad|><|vision_end|> token format
//   - Tools:  Hermes-style JSON tool call format
//             <tool_call>{"name": "...", "arguments": {...}}</tool_call>
// ─────────────────────────────────────────────────────────────────────────────
class Qwen25Adapter : public ModelAdapter {
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
    std::string adapterName() const override { return "Qwen25Adapter"; }
    bool supportsVision() const override { return true; }
    bool supportsToolCalling() const override { return true; }
};
