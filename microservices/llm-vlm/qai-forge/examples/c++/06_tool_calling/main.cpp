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
//   - StandardResponse.tool_calls — parsed by the model's ModelAdapter
//   - Tool result message (role: "tool") in the follow-up request
//   - QaiForge::generate() / generateStream() facade (same API as 01-05)
//
// Note: Tool calling requires a model that supports it (e.g. Qwen2.5-7B-Instruct).
//
// Model selection (checked in order):
//   1. argv[1]              e.g. ./06_tool_calling qwen2.5-7b
//   2. QAI_FORGE_MODEL env var
//   3. Fallback default: "qwen2.5-7b"
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/QaiForge.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/InternalDTOs.h"
#include <iostream>
#include <string>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <condition_variable>
#include <mutex>
#include <cstdlib>

namespace {

std::string resolveModelId(int argc, char* argv[], const std::string& fallback) {
    if (argc > 1 && argv[1][0] != '\0') {
        return argv[1];
    }
    if (const char* env = std::getenv("QAI_FORGE_MODEL")) {
        if (env[0] != '\0') return env;
    }
    return fallback;
}

// ── Simulated tool implementations ────────────────────────────────────────────

std::string get_weather(const std::string& location, const std::string& unit) {
    // Simulated weather data — in production, call a real weather API
    std::ostringstream oss;
    oss << "{\"location\":\"" << location << "\","
        << "\"temperature\":" << (unit == "fahrenheit" ? 72 : 22) << ","
        << "\"unit\":\"" << unit << "\","
        << "\"condition\":\"Partly cloudy\","
        << "\"humidity\":65}";
    return oss.str();
}

std::string calculate(const std::string& expression) {
    // Very simple calculator — only handles basic arithmetic for demo purposes
    // In production, use a proper expression parser
    std::ostringstream oss;
    oss << "{\"expression\":\"" << expression << "\","
        << "\"result\":\"(calculated)\","
        << "\"note\":\"Use a real expression parser in production\"}";
    return oss.str();
}

// Dispatch a tool call to the appropriate implementation
std::string execute_tool(const std::string& tool_name, const json& arguments) {
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

} // namespace

int main(int argc, char* argv[]) {
    const std::string model_id = resolveModelId(argc, argv, "qwen2.5-7b");

    std::cout << "=== qai-forge Example 06: Tool Calling ===\n\n";
    std::cout << "Model: " << model_id
               << " (override with argv[1] or QAI_FORGE_MODEL env var)\n\n";

    qai_forge::QaiForge::getInstance().start();

    // Initialize model registry
    ModelConfigManager::getInstance().scanModelBundles();
    if (!ModelConfigManager::getInstance().validateModel(model_id)) {
        std::cerr << "ERROR: Model '" << model_id << "' not found.\n";
        qai_forge::QaiForge::getInstance().shutdown();
        return 1;
    }

    // ── Define tools ───────────────────────────────────────────────────────────
    // Tools are defined in the OpenAI function calling format.
    // The model's adapter formats these into the model-specific system
    // prompt injection.
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

    int exit_code = 0;
    try {
        StandardResponse turn1_response =
            qai_forge::QaiForge::getInstance().generate(turn1_request);

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

            std::mutex done_mutex;
            std::condition_variable done_cv;
            bool done = false;
            bool had_error = false;
            std::string error_message;

            qai_forge::StreamCallbacks callbacks;
            callbacks.onToken = [&](const StreamChunk& chunk) {
                if (chunk.content_delta.has_value()) {
                    std::cout << chunk.content_delta.value();
                    std::cout.flush();
                }
            };
            callbacks.onComplete = [&](const StandardResponse&) {
                std::lock_guard<std::mutex> lock(done_mutex);
                done = true;
                done_cv.notify_one();
            };
            callbacks.onError = [&](const GenAIException& e) {
                std::lock_guard<std::mutex> lock(done_mutex);
                had_error = true;
                error_message = e.message;
                done = true;
                done_cv.notify_one();
            };
            callbacks.onCancelled = [&]() {
                std::lock_guard<std::mutex> lock(done_mutex);
                had_error = true;
                error_message = "Request cancelled";
                done = true;
                done_cv.notify_one();
            };

            qai_forge::QaiForge::getInstance().generateStream(turn2_request, callbacks);
            {
                std::unique_lock<std::mutex> lock(done_mutex);
                done_cv.wait(lock, [&] { return done; });
            }

            std::cout << "\n────────────────────────────────────────────────────────\n";
            if (had_error) {
                std::cerr << "GenAI error: " << error_message << "\n";
                exit_code = 1;
            }

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
        exit_code = 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        exit_code = 1;
    }

    qai_forge::QaiForge::getInstance().shutdown();
    return exit_code;
}
