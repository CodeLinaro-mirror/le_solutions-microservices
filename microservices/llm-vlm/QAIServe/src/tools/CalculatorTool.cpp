// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "tools/CalculatorTool.h"
#include <cmath>
#include <cctype>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <limits>
#include <algorithm>

std::string CalculatorTool::name() const { return "calculator"; }

std::string CalculatorTool::description() const {
    return
        "Evaluate a mathematical expression and return the numeric result. "
        "Use this tool for any arithmetic or mathematical computation. "
        "Supported: +, -, *, /, ^ (exponentiation), parentheses, "
        "functions: sqrt, abs, floor, ceil, round, sin, cos, tan, log, log10, "
        "constants: pi, e. "
        "Examples: '15 * 7 + 42', 'sqrt(144)', '(2^10) - 1', 'sin(pi/2)'.";
}

json CalculatorTool::inputSchema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"expression", {
                {"type", "string"},
                {"description", "Mathematical expression to evaluate, e.g. '15 * 7 + 42'"}
            }}
        }},
        {"required", {"expression"}}
    };
}

McpToolResult CalculatorTool::execute(const json& arguments) {
    if (!arguments.contains("expression") || !arguments["expression"].is_string())
        return makeErrorResult("Missing required argument 'expression'.");

    std::string expression = arguments["expression"].get<std::string>();
    if (expression.empty()) return makeErrorResult("Expression is empty.");

    std::string error;
    double result = evaluate(expression, error);
    if (!error.empty())
        return makeErrorResult("Could not evaluate '" + expression + "': " + error);

    std::string result_str;
    if (std::isfinite(result) && result == std::floor(result) && std::abs(result) < 1e15) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(0) << result;
        result_str = oss.str();
    } else if (std::isinf(result)) {
        result_str = (result > 0) ? "Infinity" : "-Infinity";
    } else if (std::isnan(result)) {
        result_str = "NaN";
    } else {
        std::ostringstream oss;
        oss << std::setprecision(10) << result;
        result_str = oss.str();
        if (result_str.find('.') != std::string::npos) {
            result_str.erase(result_str.find_last_not_of('0') + 1);
            if (result_str.back() == '.') result_str.pop_back();
        }
    }

    json output = {{"expression", expression}, {"result", result}, {"result_str", result_str}};
    return makeTextResult(output.dump());
}

double CalculatorTool::evaluate(const std::string& expression, std::string& error) {
    error.clear();
    if (expression.empty()) { error = "Empty expression"; return 0.0; }
    try {
        Parser p{expression, 0};
        double result = p.parseExpression();
        p.skipWhitespace();
        if (!p.atEnd()) {
            error = std::string("Unexpected character '") + p.peek() + "'";
            return 0.0;
        }
        return result;
    } catch (const std::exception& e) { error = e.what(); return 0.0; }
}

void CalculatorTool::Parser::skipWhitespace() {
    while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) ++pos;
}
char CalculatorTool::Parser::peek() const { return pos >= input.size() ? '\0' : input[pos]; }
char CalculatorTool::Parser::consume() {
    if (pos >= input.size()) throw std::runtime_error("Unexpected end of expression");
    return input[pos++];
}
bool CalculatorTool::Parser::atEnd() const { return pos >= input.size(); }

double CalculatorTool::Parser::parseExpression() {
    double result = parseTerm(); skipWhitespace();
    while (!atEnd() && (peek() == '+' || peek() == '-')) {
        char op = consume(); skipWhitespace(); double rhs = parseTerm();
        if (op == '+') result += rhs; else result -= rhs; skipWhitespace();
    }
    return result;
}

double CalculatorTool::Parser::parseTerm() {
    double result = parseFactor(); skipWhitespace();
    while (!atEnd() && (peek() == '*' || peek() == '/')) {
        char op = consume(); skipWhitespace(); double rhs = parseFactor();
        if (op == '*') result *= rhs;
        else { if (rhs == 0.0) throw std::runtime_error("Division by zero"); result /= rhs; }
        skipWhitespace();
    }
    return result;
}

double CalculatorTool::Parser::parseFactor() {
    double base = parseBase(); skipWhitespace();
    if (!atEnd() && peek() == '^') {
        consume(); skipWhitespace(); return std::pow(base, parseFactor());
    }
    return base;
}

double CalculatorTool::Parser::parseBase() {
    skipWhitespace();
    if (atEnd()) throw std::runtime_error("Unexpected end of expression");
    char c = peek();
    if (c == '-') { consume(); skipWhitespace(); return -parseBase(); }
    if (c == '+') { consume(); skipWhitespace(); return parseBase(); }
    if (c == '(') {
        consume(); skipWhitespace();
        double result = parseExpression(); skipWhitespace();
        if (atEnd() || peek() != ')') throw std::runtime_error("Missing ')'");
        consume(); return result;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') return parseNumber();
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
        std::string ident = parseIdentifier(); skipWhitespace();
        if (ident == "pi" || ident == "PI") return M_PI;
        if (ident == "e"  || ident == "E")  return M_E;
        if (ident == "inf" || ident == "infinity") return std::numeric_limits<double>::infinity();
        if (!atEnd() && peek() == '(') {
            consume(); skipWhitespace();
            double arg = parseExpression(); skipWhitespace();
            if (atEnd() || peek() != ')') throw std::runtime_error("Missing ')' for " + ident);
            consume();
            if (ident == "sqrt")  return std::sqrt(arg);
            if (ident == "abs")   return std::abs(arg);
            if (ident == "floor") return std::floor(arg);
            if (ident == "ceil")  return std::ceil(arg);
            if (ident == "round") return std::round(arg);
            if (ident == "sin")   return std::sin(arg);
            if (ident == "cos")   return std::cos(arg);
            if (ident == "tan")   return std::tan(arg);
            if (ident == "asin")  return std::asin(arg);
            if (ident == "acos")  return std::acos(arg);
            if (ident == "atan")  return std::atan(arg);
            if (ident == "log")   { if (arg<=0) throw std::runtime_error("log() arg must be positive"); return std::log(arg); }
            if (ident == "log10") { if (arg<=0) throw std::runtime_error("log10() arg must be positive"); return std::log10(arg); }
            if (ident == "log2")  { if (arg<=0) throw std::runtime_error("log2() arg must be positive"); return std::log2(arg); }
            if (ident == "exp")   return std::exp(arg);
            if (ident == "sign")  return (arg > 0) ? 1.0 : (arg < 0) ? -1.0 : 0.0;
            throw std::runtime_error("Unknown function '" + ident + "'");
        }
        throw std::runtime_error("Unknown identifier '" + ident + "'");
    }
    throw std::runtime_error(std::string("Unexpected character '") + c + "'");
}

double CalculatorTool::Parser::parseNumber() {
    size_t start = pos; bool has_dot = false, has_exp = false;
    while (!atEnd()) {
        char c = peek();
        if (std::isdigit(static_cast<unsigned char>(c))) { ++pos; }
        else if (c == '.' && !has_dot && !has_exp) { has_dot = true; ++pos; }
        else if ((c == 'e' || c == 'E') && !has_exp && pos > start) {
            has_exp = true; ++pos;
            if (!atEnd() && (peek() == '+' || peek() == '-')) ++pos;
        } else break;
    }
    if (pos == start) throw std::runtime_error("Expected number");
    try { return std::stod(input.substr(start, pos - start)); }
    catch (...) { throw std::runtime_error("Invalid number"); }
}

std::string CalculatorTool::Parser::parseIdentifier() {
    size_t start = pos;
    while (!atEnd() && (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')) ++pos;
    if (pos == start) throw std::runtime_error("Expected identifier");
    return input.substr(start, pos - start);
}
