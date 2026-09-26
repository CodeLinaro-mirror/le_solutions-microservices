// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

using json = nlohmann::ordered_json;

// ─────────────────────────────────────────────────────────────────────────────
// ModelAdapter — Abstract Interface
//
// Defines the contract for all model-specific transformations.
// The core orchestration pipeline (ChatOrchestratorImpl) calls these methods
// without knowing which model family it is dealing with.
//
// This eliminates all `if (model_id.contains("qwen2.5"))` checks from the
// orchestration layer. When a new model is introduced, only a new adapter
// class needs to be written — the pipeline remains untouched.
//
// Resolved by ModelAdapterFactory at the start of each pipeline execution.
// ─────────────────────────────────────────────────────────────────────────────
class ModelAdapter {
public:
    virtual ~ModelAdapter() = default;

    // ── Vision Preprocessing ───────────────────────────────────────────────────

    /**
     * Preprocess messages for vision models.
     * Transforms image_url content parts into the model-specific token format.
     *
     * Vision start/end markers are read from chat_template's "vision_start"/
     * "vision_end" fields when present, falling back to adapter-specific
     * defaults otherwise.
     *
     * For LLM (non-vision) models: returns messages unchanged.
     *
     * @param messages       Raw OpenAI-format messages array
     * @param chat_template  Chat template config from ModelConfigManager
     *                       (may be empty/omitted for non-vision call sites)
     * @param extra_images   Count of images supplied out-of-band (e.g. OIP
     *                       raw_images/images_shm) that have no corresponding
     *                       image_url content part in `messages`. When > 0,
     *                       the adapter must still inject one vision marker
     *                       pair into the current turn's text so the VLM
     *                       backend's interleaved-image prompt path fires.
     * @return               Transformed messages with model-specific image tokens
     */
    virtual json preprocessVision(const json& messages,
                                  const json& chat_template = json::object(),
                                  int extra_images = 0) const = 0;

    // ── Tool Calling ───────────────────────────────────────────────────────────

    /**
     * Format tool definitions into a system prompt injection string.
     * This string is prepended to the system prompt to instruct the model
     * about available tools and the expected JSON call format.
     *
     * For Qwen 2.5: uses Hermes-style tool JSON wrapped in <tool_call> tags.
     * For Qwen 3:   solicits bare JSON ({"name":...,"arguments":...}).
     * For models without tool support: returns empty string
     *
     * @param tools  OpenAI-format tools array from the request
     * @return       Model-specific tool instruction string
     */
    virtual std::string formatToolInstructions(const json& tools) const = 0;

    /**
     * Parse tool calls from the model's raw text response.
     * Different models emit tool calls in different formats.
     *
     * For Qwen 2.5: parses <tool_call>{"name":...,"arguments":...}</tool_call>
     * For Qwen 3:   checks for <tool_call> tags first, then falls back to
     *               extracting bare JSON objects via ToolCallJsonUtils.
     *
     * @param response_text  Raw text output from the model
     * @return               OpenAI-format tool_calls array, or empty array if none
     */
    virtual json parseToolCalls(const std::string& response_text) const = 0;

    /**
     * Format tool results into the model-specific input format.
     * Called when building the context for a tool continuation turn.
     *
     * @param tool_results  Array of tool result messages (role: "tool")
     * @return              Model-specific formatted tool result string
     */
    virtual std::string formatToolResponse(const json& tool_results) const = 0;

    // ── System Prompt ──────────────────────────────────────────────────────────

    /**
     * Build the complete system prompt, combining the user-provided system
     * message with any model-specific injections (e.g., tool instructions,
     * thinking budget hints).
     *
     * @param chat_template  Chat template config from ModelConfigManager
     * @param user_system    User-provided system message content (may be empty)
     * @param tools          Optional tools array (for tool instruction injection)
     * @return               Complete system prompt string
     */
    virtual std::string buildSystemPrompt(const json& chat_template,
                                          const std::string& user_system,
                                          const json& tools) const = 0;

    // ── Model Identity ─────────────────────────────────────────────────────────

    /**
     * Returns the model family name for logging.
     */
    virtual std::string adapterName() const = 0;

    /**
     * Returns true if this adapter supports vision preprocessing.
     */
    virtual bool supportsVision() const { return false; }

    /**
     * Returns true if this adapter supports tool calling.
     */
    virtual bool supportsToolCalling() const { return false; }

    /**
     * Returns the substring that marks the start of a tool-call payload in
     * this model's raw output (e.g. "<tool_call>" for Qwen3, "<|tool_call>"
     * for Gemma's LiteRT-LM FC grammar). Used only to gate per-token
     * streaming suppression in orchestrators that stream raw tokens as they
     * arrive: once this marker appears in the accumulated response text,
     * raw tokens must be buffered (not forwarded to the client) rather than
     * streamed, since the tool-call payload can only be safely parsed once
     * complete (via parseToolCalls() on the full text).
     *
     * IMPORTANT: this must NOT be conflated with supportsToolCalling() — an
     * adapter can support tool calling while returning "" here if
     * suppression isn't needed, and conversely an orchestrator must only
     * suppress once this marker is actually observed in the response, not
     * merely because the adapter is tool-calling-capable in general.
     *
     * Returns "" (no suppression) for adapters that don't support tool
     * calling, or whose tool-call syntax doesn't require full-response
     * buffering to detect.
     */
    virtual std::string toolCallStartMarker() const { return ""; }
};
