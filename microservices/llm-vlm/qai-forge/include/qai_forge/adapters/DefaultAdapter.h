// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/adapters/ModelAdapter.h"

// ─────────────────────────────────────────────────────────────────────────────
// DefaultAdapter — Fallback for models without special requirements
//
// Used when no specific adapter is registered for a model family.
// Passes messages through unchanged and returns empty strings for
// tool/vision operations.
// ─────────────────────────────────────────────────────────────────────────────
class DefaultAdapter : public ModelAdapter {
public:
    json preprocessVision(const json& messages) const override { return messages; }
    std::string formatToolInstructions(const json& tools) const override { return ""; }
    json parseToolCalls(const std::string& response_text) const override { return json::array(); }
    std::string formatToolResponse(const json& tool_results) const override { return ""; }

    std::string buildSystemPrompt(const json& chat_template,
                                  const std::string& user_system,
                                  const json& tools) const override {
        return user_system;
    }

    std::string adapterName() const override { return "DefaultAdapter"; }
    bool supportsVision() const override { return false; }
    bool supportsToolCalling() const override { return false; }
};
