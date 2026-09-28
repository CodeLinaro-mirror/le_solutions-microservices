// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/adapters/GemmaAdapter.h"
#include "qai_forge/adapters/ToolCallJsonUtils.h"
#include "qai_forge/utils/Logger.h"
#include <cctype>
#include <random>
#include <sstream>
#include <stdexcept>

namespace {

std::string generateToolCallId() {
    static std::mt19937_64 rng(std::random_device{}());
    std::ostringstream oss;
    oss << "call_" << std::hex << rng();
    return oss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// FcParser — recursive-descent parser for LiteRT-LM's "FC" tool-call grammar:
//
//   start        := functionCall
//   functionCall := 'call' ':' ID object?
//   object       := '{' ( pair (',' pair)* )? '}'
//   pair         := ID ':' value
//   value        := ESCAPED_STRING | NUMBER | BOOLEAN | NULL_LITERAL
//                 | object | array
//   array        := '[' ( value (',' value)* )? ']'
//   ESCAPED_STRING := ('<escape>' | '<|"|>') .*? ('<escape>' | '<|"|>')
// ─────────────────────────────────────────────────────────────────────────────
class FcParser {
public:
    explicit FcParser(const std::string& text) : text_(text), pos_(0) {}

    json parseFunctionCall() {
        skipWhitespace();
        expectLiteral("call");
        skipWhitespace();
        expectChar(':');
        skipWhitespace();
        std::string name = parseIdentifier();
        if (name.empty()) {
            throw std::runtime_error("FcParser: missing tool name after 'call:'");
        }

        json arguments = json::object();
        skipWhitespace();
        if (peek() == '{') {
            arguments = parseObject();
        }

        json result = json::object();
        result["name"] = name;
        result["arguments"] = arguments;
        return result;
    }

    std::size_t position() const { return pos_; }

private:
    const std::string& text_;
    std::size_t pos_;

    char peek() const {
        return pos_ < text_.size() ? text_[pos_] : '\0';
    }

    void skipWhitespace() {
        while (pos_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
    }

    void expectChar(char c) {
        if (peek() != c) {
            throw std::runtime_error(
                std::string("FcParser: expected '") + c + "' at position " +
                std::to_string(pos_));
        }
        ++pos_;
    }

    void expectLiteral(const std::string& literal) {
        if (text_.compare(pos_, literal.size(), literal) != 0) {
            throw std::runtime_error(
                "FcParser: expected literal '" + literal + "' at position " +
                std::to_string(pos_));
        }
        pos_ += literal.size();
    }

    bool startsWithAt(std::size_t at, const std::string& literal) const {
        return text_.compare(at, literal.size(), literal) == 0;
    }

    std::string parseIdentifier() {
        std::size_t start = pos_;
        if (pos_ >= text_.size() ||
            !(std::isalpha(static_cast<unsigned char>(text_[pos_])) ||
              text_[pos_] == '_')) {
            return "";
        }
        ++pos_;
        while (pos_ < text_.size() &&
               (std::isalnum(static_cast<unsigned char>(text_[pos_])) ||
                text_[pos_] == '_' || text_[pos_] == '.' ||
                text_[pos_] == '-')) {
            ++pos_;
        }
        return text_.substr(start, pos_ - start);
    }

    json parseObject() {
        expectChar('{');
        json obj = json::object();
        skipWhitespace();
        if (peek() == '}') {
            ++pos_;
            return obj;
        }
        while (true) {
            skipWhitespace();
            std::string key = parseIdentifier();
            if (key.empty()) {
                throw std::runtime_error(
                    "FcParser: expected key at position " +
                    std::to_string(pos_));
            }
            skipWhitespace();
            expectChar(':');
            skipWhitespace();
            json value = parseValue();
            obj[key] = std::move(value);
            skipWhitespace();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            break;
        }
        skipWhitespace();
        expectChar('}');
        return obj;
    }

    json parseArray() {
        expectChar('[');
        json arr = json::array();
        skipWhitespace();
        if (peek() == ']') {
            ++pos_;
            return arr;
        }
        while (true) {
            skipWhitespace();
            arr.push_back(parseValue());
            skipWhitespace();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            break;
        }
        skipWhitespace();
        expectChar(']');
        return arr;
    }

    json parseValue() {
        skipWhitespace();
        char c = peek();

        if (c == '{') {
            return parseObject();
        }
        if (c == '[') {
            return parseArray();
        }
        if (startsWithAt(pos_, "<escape>") || startsWithAt(pos_, "<|\"|>")) {
            return json(parseEscapedString());
        }
        if (startsWithAt(pos_, "true")) {
            pos_ += 4;
            return json(true);
        }
        if (startsWithAt(pos_, "false")) {
            pos_ += 5;
            return json(false);
        }
        if (startsWithAt(pos_, "null")) {
            pos_ += 4;
            return json(nullptr);
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            return parseNumber();
        }

        throw std::runtime_error(
            "FcParser: unrecognized value at position " +
            std::to_string(pos_));
    }

    std::string parseEscapedString() {
        std::string open_tag;
        if (startsWithAt(pos_, "<escape>")) {
            open_tag = "<escape>";
        } else if (startsWithAt(pos_, "<|\"|>")) {
            open_tag = "<|\"|>";
        } else {
            throw std::runtime_error(
                "FcParser: expected escape tag at position " +
                std::to_string(pos_));
        }
        pos_ += open_tag.size();

        std::size_t content_start = pos_;
        while (pos_ < text_.size()) {
            if (startsWithAt(pos_, "<escape>")) {
                std::string content =
                    text_.substr(content_start, pos_ - content_start);
                pos_ += 8;
                return content;
            }
            if (startsWithAt(pos_, "<|\"|>")) {
                std::string content =
                    text_.substr(content_start, pos_ - content_start);
                pos_ += 5;
                return content;
            }
            ++pos_;
        }
        throw std::runtime_error("FcParser: unterminated escaped string");
    }

    json parseNumber() {
        std::size_t start = pos_;
        if (peek() == '-') ++pos_;
        bool has_digits = false;
        while (pos_ < text_.size() &&
               std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
            has_digits = true;
        }
        bool is_float = false;
        if (peek() == '.') {
            is_float = true;
            ++pos_;
            while (pos_ < text_.size() &&
                   std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                ++pos_;
                has_digits = true;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            is_float = true;
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            while (pos_ < text_.size() &&
                   std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
                ++pos_;
            }
        }
        if (!has_digits) {
            throw std::runtime_error(
                "FcParser: malformed number at position " +
                std::to_string(start));
        }
        std::string literal = text_.substr(start, pos_ - start);
        if (!is_float) {
            try {
                return json(static_cast<int64_t>(std::stoll(literal)));
            } catch (const std::exception&) {
            }
        }
        return json(std::stod(literal));
    }
};

json toOpenAiToolCall(const json& fc_call) {
    json tool_call = json::object();
    tool_call["id"] = generateToolCallId();
    tool_call["type"] = "function";
    json function = json::object();
    function["name"] = fc_call.value("name", "");
    function["arguments"] = fc_call.value(
        "arguments", json::object()).dump();
    tool_call["function"] = std::move(function);
    return tool_call;
}

} // namespace

json GemmaAdapter::parseToolCalls(const std::string& response_text) const {
    return parseToolCalls(response_text, json::array());
}

json GemmaAdapter::parseToolCalls(const std::string& response_text,
                                   const json& tools) const {
    json tool_calls = json::array();

    static const std::string kOpenTag = "<|tool_call>";
    static const std::string kCloseTag = "<tool_call|>";

    std::size_t pos = 0;
    while (true) {
        std::size_t open_pos = response_text.find(kOpenTag, pos);
        if (open_pos == std::string::npos) {
            break;
        }
        std::size_t content_start = open_pos + kOpenTag.size();
        std::size_t close_pos = response_text.find(kCloseTag, content_start);
        if (close_pos == std::string::npos) {
            LOG_WARN("[GemmaAdapter] Found opening tag without matching "
                     "closing tag — stopping scan");
            break;
        }

        std::string call_text = response_text.substr(
            content_start, close_pos - content_start);
        try {
            FcParser parser(call_text);
            json fc_call = parser.parseFunctionCall();
            tool_calls.push_back(toOpenAiToolCall(fc_call));
        } catch (const std::exception& e) {
            LOG_WARN("[GemmaAdapter] Failed to parse FC tool call: "
                     << e.what());
        }

        pos = close_pos + kCloseTag.size();
    }

    if (tool_calls.empty()) {
        std::size_t call_pos = response_text.find("call:");
        if (call_pos != std::string::npos &&
            (call_pos == 0 ||
             std::isspace(static_cast<unsigned char>(
                 response_text[call_pos - 1])))) {
            try {
                std::string suffix = response_text.substr(call_pos);
                FcParser suffix_parser(suffix);
                json fc_call = suffix_parser.parseFunctionCall();
                tool_calls.push_back(toOpenAiToolCall(fc_call));
            } catch (const std::exception& e) {
                LOG_WARN("[GemmaAdapter] Failed to parse bare FC tool call: "
                         << e.what());
            }
        }
    }

    // Case-insensitive tool-name remap / rejection against the caller's
    // declared tools. Calls for names that don't match any declared tool
    // are dropped rather than silently passed through with an
    // unrecognized name.
    if (!tool_calls.empty() && tools.is_array() && !tools.empty()) {
        json filtered = json::array();
        for (auto& call : tool_calls) {
            std::string called_name =
                call.value("function", json::object()).value("name", "");
            auto normalized =
                ToolCallJsonUtils::normalizeToolName(called_name, tools);
            if (!normalized.has_value()) {
                LOG_WARN("[GemmaAdapter] Dropping tool call for "
                         "unrecognized tool name: " << called_name);
                continue;
            }
            if (normalized.value() != called_name) {
                call["function"]["name"] = normalized.value();
            }
            filtered.push_back(std::move(call));
        }
        tool_calls = std::move(filtered);
    }

    return tool_calls;
}
