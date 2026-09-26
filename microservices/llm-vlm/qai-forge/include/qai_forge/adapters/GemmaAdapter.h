// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/adapters/ModelAdapter.h"

// ─────────────────────────────────────────────────────────────────────────────
// GemmaAdapter — Gemma Model Family (LiteRT-LM FC tool-call syntax)
//
// Handles model-specific transformations for Gemma models run via the
// LiteRT-LM backend. Gemma's own chat template renders tool calls using
// LiteRT-LM's native "FC" (function-call) grammar, wrapped in asymmetric
// channel tags:
//
//   <|tool_call>call:NAME{key:<|"|>value<|"|>,key2:123}<tool_call|>
//
// Only parseToolCalls() has a real implementation — LiteRTLMOrchestrator
// renders prompts via the model's own embedded Jinja template, so
// formatToolInstructions()/buildSystemPrompt()/formatToolResponse() are
// never called for LiteRT-LM-backed models and simply passthrough/no-op
// here (matching DefaultAdapter).
// ─────────────────────────────────────────────────────────────────────────────
class GemmaAdapter : public ModelAdapter {
public:
    json preprocessVision(const json& messages,
                          const json& chat_template = json::object(),
                          int extra_images = 0) const override {
        (void)chat_template;
        (void)extra_images;
        return messages;
    }

    std::string formatToolInstructions(const json& tools) const override {
        (void)tools;
        return "";
    }

    json parseToolCalls(const std::string& response_text) const override;

    std::string formatToolResponse(const json& tool_results) const override {
        (void)tool_results;
        return "";
    }

    std::string buildSystemPrompt(const json& chat_template,
                                  const std::string& user_system,
                                  const json& tools) const override {
        (void)chat_template;
        (void)tools;
        return user_system;
    }

    std::string adapterName() const override { return "GemmaAdapter"; }
    bool supportsVision() const override { return false; }
    bool supportsToolCalling() const override { return true; }
    std::string toolCallStartMarker() const override { return "<|tool_call>"; }
};
