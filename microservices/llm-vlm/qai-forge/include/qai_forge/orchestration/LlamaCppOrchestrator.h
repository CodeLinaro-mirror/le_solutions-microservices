// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#ifdef QAI_FORGE_BUILD_LLAMACPP

#include "qai_forge/orchestration/IGenerativeOrchestrator.h"
#include "qai_forge/InternalDTOs.h"
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::ordered_json;

namespace qai_forge {

/**
 * @brief Stateless orchestrator for llama.cpp backend
 *
 * Implements IGenerativeOrchestrator with backend injection.
 * The backend is passed as a parameter to execute() — this orchestrator
 * does NOT store a backend reference.
 *
 * Design:
 * - Stateless: No member variables for backend, session, or state
 * - Backend Injection: Backend passed to execute(), not stored
 * - Response History Merging: Merges options response_history into request.messages
 * - Direct Message Passing: Passes messages to llama-server (no Jinja in C++)
 * - SSE Parsing: Parses Server-Sent Events from llama-server
 * - Callback Invocation: Calls OrchestratorStreamCallback with StreamChunk
 * - StandardResponse Return: Returns complete response even in streaming mode
 *
 * Pattern follows LiteRTLMOrchestrator implementation.
 */
class LlamaCppOrchestrator : public IGenerativeOrchestrator {
public:
    LlamaCppOrchestrator() = default;
    ~LlamaCppOrchestrator() override = default;

    /**
     * Execute one inference turn.
     *
     * The backend is injected as a parameter — this orchestrator does NOT
     * store a backend reference. This follows the stateless orchestrator
     * pattern where ModelRuntime owns the backend and passes it per call.
     *
     * @param request          Chat completion request (model, messages, params)
     * @param options          Scheduler invoke options
     * @param backend          Backend instance (injected, not stored)
     * @param callback         Stream callback (nullptr for blocking mode)
     * @param cancel           Cancellation predicate
     * @return                 StandardResponse with content, tool_calls, usage
     */
    scheduler::GenerativeJobPtr createJob(
        scheduler::GenerativeJobContext context,
        scheduler::GenerativeCallbacks callbacks) const override;

    StandardResponse execute(
        scheduler::GenerativeJob& job,
        IGenerativeBackend& backend) const override;

private:
    /**
     * @brief Build OpenAI chat completions JSON request
     * @param request Original request
     * @param merged_messages Messages with response_history merged in
     * @return JSON request body for /v1/chat/completions
     */
    json buildChatCompletionsRequest(
        const CreateChatCompletionRequest& request,
        json merged_messages) const;

    /**
     * @brief Parse SSE chunk and invoke callback
     * @param sse_chunk Raw SSE data ("data: {...}\n\n")
     * @param callback Stream callback to invoke
     * @param event_id Event ID for StreamChunk
     * @param model Model name for StreamChunk
     * @param accumulated_content Reference to accumulate content
     * @param accumulated_tool_calls Reference to accumulate tool calls
     * @param finish_reason Reference to store finish reason
     * @param prompt_tokens Reference to count prompt tokens
     * @param completion_tokens Reference to count completion tokens
     */
    void handleSseChunk(
        const std::string& sse_chunk,
        const std::function<void(const StreamChunk&)>& callback,
        const std::string& event_id,
        const std::string& model,
        std::string& accumulated_content,
        json& accumulated_tool_calls,
        std::string& finish_reason,
        int& prompt_tokens,
        int& completion_tokens) const;

    /**
     * @brief Build StandardResponse from accumulated data
     * @param event_id Event ID
     * @param model Model name
     * @param accumulated_content Accumulated content
     * @param accumulated_tool_calls Accumulated tool calls
     * @param finish_reason Finish reason
     * @param prompt_tokens Prompt token count
     * @param completion_tokens Completion token count
     * @return StandardResponse
     */
    StandardResponse buildStandardResponse(
        const std::string& event_id,
        const std::string& model,
        const std::string& accumulated_content,
        const json& accumulated_tool_calls,
        const std::string& finish_reason,
        int prompt_tokens,
        int completion_tokens) const;
};

} // namespace qai_forge

#endif // QAI_FORGE_BUILD_LLAMACPP
