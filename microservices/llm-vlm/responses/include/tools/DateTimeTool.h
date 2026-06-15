// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// DateTimeTool.h — Native tool: current date and time
//
// Tool name:    "datetime"
// Namespaced:   "native__datetime" (as seen by the model)
// ─────────────────────────────────────────────────────────────────────────────

#include "mcp/ITool.h"

class DateTimeTool : public ITool {
public:
    DateTimeTool() = default;
    ~DateTimeTool() override = default;

    std::string name() const override;
    std::string description() const override;
    json inputSchema() const override;
    McpToolResult execute(const json& arguments) override;
};
