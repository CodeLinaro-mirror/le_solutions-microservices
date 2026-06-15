// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once
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
