// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// McpAgenticLoop.h — Autonomous MCP tool-call loop
//
// Drives the inference → tool-call → result → re-infer cycle autonomously.
// The Responses API client does NOT need to re-submit tool results; this loop
// handles the entire multi-step interaction internally.
//
// Loop algorithm:
//   1. Submit request to ModelScheduler with MCP tools
//   2. If finish_reason == "tool_calls":
//        a. For each tool_call in the response:
//             - Resolve server via McpClientRegistry
//             - Call the tool
//             - Record the McpCallRecord
//        b. Append assistant message + tool results to messages
//        c. Go to step 1 (up to max_iterations)
//   3. If finish_reason == "stop" or "length": return final response
//
// Streaming variant:
//   Same algorithm but emits SSE events at each step:
//     response.mcp_call.in_progress  — before each tool call
//     response.mcp_call.completed    — after successful tool call
//     response.mcp_call.failed       — after failed tool call
//     response.output_text.delta     — for each token in the final answer
//
// Layer boundary:
//   McpAgenticLoop calls ModelScheduler for model inference.
//   It never touches InferenceWorkerManager directly.
//   It is called by ResponsesController (Layer 1).
// ─────────────────────────────────────────────────────────────────────────────

#include "mcp/McpClientRegistry.h"
#include "mcp/McpTypes.h"
#include "qai_forge/InternalDTOs.h"
#include <functional>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// McpLoopResult — returned by McpAgenticLoop::run()
// ─────────────────────────────────────────────────────────────────────────────
struct McpLoopResult {
    StandardResponse          final_response;
    std::vector<McpCallRecord> call_records;    // All tool calls made
    int                        iterations = 0;  // Number of inference rounds
    bool                       truncated  = false; // true if max_iterations hit
};

// ─────────────────────────────────────────────────────────────────────────────
// SSE event emitter callback type (used by runStreaming)
// ─────────────────────────────────────────────────────────────────────────────
using McpSseEmitter = std::function<void(const std::string& event_type,
                                          const json& data)>;

// ─────────────────────────────────────────────────────────────────────────────
// McpAgenticLoop
// ─────────────────────────────────────────────────────────────────────────────
class McpAgenticLoop {
public:
    /**
     * @param registry       MCP client registry (for tool calls)
     * @param max_iterations Maximum number of inference rounds (default: 10)
     */
    explicit McpAgenticLoop(McpClientRegistry& registry,
                             int max_iterations = 10);

    // ── Blocking variant ──────────────────────────────────────────────────────

    /**
     * Run the full agentic loop synchronously.
     *
     * @param base_request   The original CreateChatCompletionRequest from the
     *                       client (messages, model, temperature, etc.)
     * @param mcp_tools      OpenAI-format function tools array (translated from
     *                       MCP schemas by McpClientRegistry::getAllTools())
     * @return               McpLoopResult with final response + call records
     * @throws GenAIException on inference failure
     * @throws McpException   on tool call failure (if stop_on_tool_error=true)
     */
    McpLoopResult run(const CreateChatCompletionRequest& base_request,
                       const json& mcp_tools,
                       const std::string& response_id = "");

    // ── Streaming variant ─────────────────────────────────────────────────────

    /**
     * Run the agentic loop with SSE event emission.
     *
     * Emits the following event types via the emitter callback:
     *   "response.mcp_call.in_progress"  — before each tool call
     *   "response.mcp_call.completed"    — after successful tool call
     *   "response.mcp_call.failed"       — after failed tool call
     *   "response.output_text.delta"     — for each token in the final answer
     *   "response.output_text.done"      — when the final answer is complete
     *
     * @param base_request   Original request from the client
     * @param mcp_tools      OpenAI-format function tools array
     * @param emitter        SSE event emitter callback
     * @param response_id    Response ID for SSE event payloads
     * @return               McpLoopResult (call_records + iterations)
     */
    McpLoopResult runStreaming(const CreateChatCompletionRequest& base_request,
                                const json& mcp_tools,
                                McpSseEmitter emitter,
                                const std::string& response_id);

    // ── Configuration ─────────────────────────────────────────────────────────

    /**
     * If true, the loop stops immediately when a tool call returns isError=true.
     * Default: false (loop continues, error is passed to the model as context).
     */
    void setStopOnToolError(bool stop) { stop_on_tool_error_ = stop; }

private:
    // ── Internal helpers ──────────────────────────────────────────────────────

    // Execute all tool_calls from a StandardResponse.
    // Appends McpCallRecords to records.
    // Returns a json array of role:"tool" messages for the next turn.
    json executeToolCalls(const json& tool_calls,
                           std::vector<McpCallRecord>& records,
                           McpSseEmitter* emitter,
                           int output_index_start);

    // Generate a unique call ID: "mcpcall_{counter}"
    std::string generateCallId();

    // ── State ─────────────────────────────────────────────────────────────────
    McpClientRegistry& registry_;
    int                max_iterations_;
    bool               stop_on_tool_error_ = false;
    int                call_counter_       = 0;
};
