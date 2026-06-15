// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once
#include "mcp/ITool.h"
#include <string>

class CalculatorTool : public ITool {
public:
    CalculatorTool() = default;
    ~CalculatorTool() override = default;

    std::string name() const override;
    std::string description() const override;
    json inputSchema() const override;
    McpToolResult execute(const json& arguments) override;

private:
    struct Parser {
        const std::string& input;
        size_t pos = 0;

        double parseExpression();
        double parseTerm();
        double parseFactor();
        double parseBase();
        double parseNumber();
        std::string parseIdentifier();
        void skipWhitespace();
        char peek() const;
        char consume();
        bool atEnd() const;
    };

    static double evaluate(const std::string& expression, std::string& error);
};
