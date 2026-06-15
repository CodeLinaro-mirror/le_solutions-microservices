// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// Example 06: Function / Tool Calling
//
// Demonstrates the tool calling flow with qai-forge:
//   Turn 1: Send user message + tool definitions
//           → Model responds with a tool_call
//   Turn 2: Execute the tool locally, send tool result back
//           → Model responds with the final answer
//
// Key concepts:
//   - tools array in CreateChatCompletionRequest
//   - StandardResponse.tool_calls — parsed by ModelAdapter::parseToolCalls()
//   - Tool result message (role: "tool") in the follow-up request
//   - ModelAdapterFactory resolves the correct tool format per model family
//
// Note: Tool calling requires a model that supports it (e.g. Qwen2.5-7B-Instruct).
//       Check ModelConfig::supports_tool_calling (set in metadata.json).
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/orchestration/ChatOrchestrator.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/InternalDTOs.h"
#include <iostream>
#include <string>
#include <cmath>
#include <sstream>
#include <iomanip>

// ── Simulated tool implementations ────────────────────────────────────────────

static std::string get_weather(const std::string& location, const std::string& unit) {
    // Simulated weather data — in production, call a real weather API
    std::ostringstream oss;
    oss << "{\"location\":\"" << location << "\","
        << "\"temperature\":" << (unit == "fahrenheit" ? 72 : 22) << ","
        << "\"unit\":\"" << unit << "\","
        << "\"condition\":\"Partly cloudy\","
        << "\"humidity\":65}";
    return oss.str();
}

static std::string calculate(const std::string& expression) {
    // Very simple calculator — only handles basic arithmetic for demo purposes
    // In production, use a proper expression parser
    std::ostringstream oss;
    oss << "{\"expression\":\"" << expression << "\","
        << "\"result\":\"(calculated)\","
        << "\"note\":\"Use a real expression parser in production\"}";
    return oss.str();
}

// Dispatch a tool call to the appropriate implementation
static std::string execute_tool(const std::string& tool_name, const json& arguments) {
    if (tool_name == "get_weather") {
        std::string location = arguments.value("location", "Unknown");
        std::string unit     = arguments.value("unit", "celsius");
        return get_weather(location, unit);
    }
    if (tool_name == "calculate") {
        std::string expr = arguments.value("expression", "");
        return calculate(expr);
    }
    return "{\"error\":\"Unknown tool: " + tool_name + "\"}";
}

int main(int argc, char* argv[]) {
    const std::string model_id = (argc > 1) ? argv[1] : "qwen2.5-7b";

    std::cout << "=== qai-forge Example 06: Tool Calling ===\n\n";
    std::cout << "Model: " << model_id << "\n\n";

    // Initialize model registry
    ModelConfigManager::getInstance().scanModelBundles();
    if (!ModelConfigManager::getInstance().validateModel(model_id)) {
        std::cerr << "ERROR: Model '" << model_id << "' not found.\n";
        return 1;
    }

    // ── Define tools ───────────────────────────────────────────────────────────
    // Tools are defined in the OpenAI function calling format.
    // The ModelAdapter (Qwen25Adapter / Qwen3Adapter) formats these into
    // the model-specific system prompt injection.
    json tools = json::array({
        {
            {"type", "function"},
            {"function", {
                {"name", "get_weather"},
                {"description", "Get the current weather for a location"},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"location", {
                            {"type", "string"},
                            {"description", "City and country, e.g. 'Paris, France'"}
                        }},
                        {"unit", {
                            {"type", "string"},
                            {"enum", {"celsius", "fahrenheit"}},
                            {"description", "Temperature unit"}
                        }}
                    }},
                    {"required", {"location"}}
                }}
            }}
        },
        {
            {"type", "function"},
            {"function", {
                {"name", "calculate"},
                {"description", "Evaluate a mathematical expression"},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"expression", {
                            {"type", "string"},
                            {"description", "Mathematical expression to evaluate, e.g. '2 + 2'"}
                        }}
                    }},
                    {"required", {"expression"}}
                }}
            }}
        }
    });

    // ── Turn 1: User message + tool definitions ────────────────────────────────
    const std::string user_message = "What's the weather like in Tokyo right now? "
                                     "Also, what is 15 * 7 + 42?";

    std::cout << "User: " << user_message << "\n\n";

    CreateChatCompletionRequest turn1_request;
    turn1_request.model = model_id;
    turn1_request.stream = false;
    turn1_request.tools = tools;
    turn1_request.messages = {
        {{"role", "system"}, {"content", "You are a helpful assistant with access to tools."}},
        {{"role", "user"},   {"content", user_message}}
    };
    turn1_request.max_completion_tokens = 512;
    turn1_request.temperature = 0.1f;  // Low temperature for deterministic tool calls

    try {
        StandardResponse turn1_response = ChatOrchestrator::getInstance().handleBlocking(turn1_request);

        // Check if the model wants to call tools
        if (turn1_response.finish_reason == "tool_calls" && turn1_response.tool_calls.has_value()) {
            const json& tool_calls = turn1_response.tool_calls.value();

            std::cout << "Model requested " << tool_calls.size() << " tool call(s):\n";

            // ── Execute each tool call ─────────────────────────────────────────
            json tool_result_messages = json::array();

            for (const auto& tc : tool_calls) {
                std::string call_id   = tc.value("id", "");
                std::string func_name = tc.value("function", json::object()).value("name", "");
                std::string args_str  = tc.value("function", json::object()).value("arguments", "{}");

                std::cout << "  → " << func_name << "(" << args_str << ")\n";

                // Parse arguments and execute the tool
                json arguments;
                try {
                    arguments = json::parse(args_str);
                } catch (...) {
                    arguments = json::object();
                }

                std::string result = execute_tool(func_name, arguments);
                std::cout << "    Result: " << result << "\n";

                // Build tool result message
                tool_result_messages.push_back({
                    {"role",         "tool"},
                    {"tool_call_id", call_id},
                    {"name",         func_name},
                    {"content",      result}
                });
            }

            std::cout << "\n";

            // ── Turn 2: Send tool results back ─────────────────────────────────
            // Build the full message history including the assistant's tool call
            // and the tool results.
            json assistant_msg = {
                {"role",       "assistant"},
                {"content",    nullptr},
                {"tool_calls", tool_calls}
            };

            CreateChatCompletionRequest turn2_request;
            turn2_request.model = model_id;
            turn2_request.stream = true;
            turn2_request.messages = json::array({
                {{"role", "system"}, {"content", "You are a helpful assistant with access to tools."}},
                {{"role", "user"},   {"content", user_message}},
                assistant_msg
            });
            // Append all tool results
            for (const auto& tr : tool_result_messages) {
                turn2_request.messages.push_back(tr);
            }
            turn2_request.max_completion_tokens = 512;
            turn2_request.temperature = 0.7f;

            std::cout << "─── Final Answer ───────────────────────────────────────\n";

            ChatOrchestrator::getInstance().handleStreaming(turn2_request,
                [](const StreamChunk& chunk) {
                    if (chunk.content_delta.has_value()) {
                        std::cout << chunk.content_delta.value();
                        std::cout.flush();
                    }
                }
            );

            std::cout << "\n────────────────────────────────────────────────────────\n";

        } else {
            // Model responded directly without tool calls
            std::cout << "─── Direct Response (no tool calls) ───────────────────\n";
            std::cout << turn1_response.content.value_or("(no content)") << "\n";
            std::cout << "────────────────────────────────────────────────────────\n";
            std::cout << "(Note: This model may not support tool calling, or the\n";
            std::cout << " request did not trigger a tool call.)\n";
        }

    } catch (const GenAIException& e) {
        std::cerr << "GenAI error (HTTP " << e.http_status << "): " << e.message << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
