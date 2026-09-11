// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// GenieOrchestrator — Genie-specific generative orchestrator implementation
//
// Implements IGenerativeOrchestrator for the Qualcomm GenIE SDK backend.
//
// Key features:
//
//   1. Universal post-inference KV reset (eager background reset):
//      backend.resetKvAsync() is called immediately after every
//      generate() / generateVlm() call. The reset runs in a background
//      thread concurrently with returning the response to the HTTP layer.
//
//   2. Slot-based prompt assembly:
//      buildContextPrompt() assembles system / summary / history / current-turn
//      slots with proportional token ceilings scaled to the model's context window.
//
//   3. Genie-specific reasoning budget calculation via ReasoningBudgetCalculator.
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/orchestration/GenieOrchestrator.h"
#include "qai_forge/backend/GenIEBackend.h"
#include "qai_forge/reasoning/ReasoningRouter.h"
#include "qai_forge/reasoning/ReasoningBudgetCalculator.h"
#include "qai_forge/adapters/ModelAdapterFactory.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/ImageUtils.h"
#include "qai_forge/utils/Logger.h"
#include <algorithm>
#include <cstdint>
#include <exception>
#include <regex>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <random>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr int DEFAULT_MAX_OUTPUT_TOKENS    = 512;
constexpr int MAX_COMPLETION_SAFETY_MARGIN = 64;

// nlohmann::json's .value(key, default) only falls back to `default` when
// the key is absent — if the key is present but explicitly null (a valid
// shape for e.g. an assistant message's "content" field when "tool_calls"
// is present), .value<std::string>() throws json::type_error.302. This
// helper treats an explicit null the same as an absent key.
std::string getStringOrDefault(const json& obj,
                                const std::string& key,
                                const std::string& def = "") {
    if (!obj.is_object() || !obj.contains(key) || obj[key].is_null()) {
        return def;
    }
    const json& val = obj[key];
    return val.is_string() ? val.get<std::string>() : def;
}

// Detects whether the CURRENT turn (request.messages) contains a
// role=="tool" message — i.e. this is Trip 2 of tool calling, where the
// model already has the tool's result and should answer in plain language
// rather than emit another tool call, unless it genuinely needs one.
bool currentTurnHasToolResponse(const json& messages) {
    if (!messages.is_array()) {
        return false;
    }
    for (const auto& msg : messages) {
        if (msg.is_object() && getStringOrDefault(msg, "role", "") == "tool") {
            return true;
        }
    }
    return false;
}

std::string generateEventId() {
    static std::mt19937_64 rng(std::random_device{}());
    std::ostringstream oss;
    oss << "evt-" << std::hex << rng();
    return oss.str();
}

json canonicalConversationMessages(const json& messages) {
    json canonical = json::array();
    if (!messages.is_array()) {
        return canonical;
    }
    for (const auto& message : messages) {
        if (!message.is_object() || message.value("role", "") == "system") {
            continue;
        }
        json clean = json::object();
        for (const auto& item : message.items()) {
            if (item.key().empty() || item.key().front() != '_') {
                clean[item.key()] = item.value();
            }
        }
        canonical.push_back(std::move(clean));
    }
    return canonical;
}

std::string canonicalPrefixDigest(const json& messages, std::size_t boundary) {
    if (!messages.is_array() || boundary > messages.size()) {
        return {};
    }
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::size_t index = 0; index < boundary; ++index) {
        const std::string serialized = messages[index].dump();
        for (const unsigned char value : serialized) {
            hash ^= value;
            hash *= 1099511628211ULL;
        }
        hash ^= static_cast<unsigned char>('\n');
        hash *= 1099511628211ULL;
    }
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

bool memoryMatchesTranscript(const scheduler::GenieMemoryState& memory,
                             const json& messages) {
    if (memory.canonical_boundary == 0) {
        return true;
    }
    return memory.canonical_boundary <= messages.size() &&
           !memory.transcript_digest.empty() &&
           memory.transcript_digest == canonicalPrefixDigest(
               messages, memory.canonical_boundary);
}

std::string messageText(const json& message) {
    auto content = message.find("content");
    if (content == message.end() || content->is_null()) {
        return {};
    }
    if (content->is_string()) {
        return content->get<std::string>();
    }
    if (!content->is_array()) {
        return content->dump();
    }

    std::ostringstream text;
    for (const auto& part : *content) {
        if (part.is_string()) {
            text << part.get<std::string>();
        } else if (part.is_object() && part.contains("text") &&
                   part["text"].is_string()) {
            text << part["text"].get<std::string>();
        }
    }
    return text.str();
}

std::string renderToolCallsForPrompt(const json& tool_calls) {
    if (!tool_calls.is_array() || tool_calls.empty()) {
        return "";
    }

    std::ostringstream oss;
    for (const auto& tool_call : tool_calls) {
        if (!tool_call.is_object()) {
            continue;
        }
        json function = tool_call.value("function", json::object());
        std::string name = function.value("name", "");
        if (name.empty()) {
            continue;
        }

        json arguments = json::object();
        std::string raw_arguments = function.value("arguments", "{}");
        try {
            arguments = json::parse(raw_arguments);
        } catch (...) {
            arguments = raw_arguments;
        }

        json protocol = {
            {"name", name},
            {"arguments", arguments}
        };
        oss << "<tool_call>\n" << protocol.dump() << "\n</tool_call>\n";
    }
    return oss.str();
}

std::vector<std::vector<uint8_t>> preprocessImagesToBuffers(
    const json& messages,
    const std::string& model_id) {
    VisionPreprocessConfig vision_cfg = VisionPreprocessConfig::defaults();
    auto opt_preprocess =
        ModelConfigManager::getInstance().getVisionPreprocessing(model_id);
    if (opt_preprocess.has_value() && !opt_preprocess->is_null()) {
        vision_cfg = VisionPreprocessConfig::fromJson(*opt_preprocess);
    }

    std::vector<std::string> image_urls;
    for (const auto& msg : messages) {
        if (!msg.is_object()) {
            continue;
        }
        const auto& content = msg.value("content", json{});
        if (!content.is_array()) {
            continue;
        }
        for (const auto& part : content) {
            if (!part.is_object()) {
                continue;
            }
            std::string type = part.value("type", "");
            if (type != "image_url" && type != "input_image") {
                continue;
            }

            auto img = part.value("image_url", json::object());
            if (img.is_string()) {
                std::string url = img.get<std::string>();
                if (!url.empty()) {
                    image_urls.push_back(url);
                }
                continue;
            }
            if (img.is_object()) {
                std::string url = img.value("url", "");
                if (!url.empty()) {
                    image_urls.push_back(url);
                }
                continue;
            }
        }
    }

    if (image_urls.size() == 1) {
        LOG_INFO("[GenieOrchestrator] Only one image supplied for VLM; "
                 "duplicating image to satisfy backend minimum");
        image_urls.push_back(image_urls[0]);
    }

    std::vector<std::vector<uint8_t>> buffers;
    for (size_t i = 0; i < image_urls.size(); ++i) {
        try {
            buffers.push_back(ImageUtils::preprocessImageToBuffer(
                image_urls[i],
                vision_cfg,
                model_id));
            LOG_INFO("[GenieOrchestrator] Preprocessed VLM image "
                     << (i + 1) << "/" << image_urls.size()
                     << ": model=" << model_id
                     << " bytes=" << buffers.back().size());
        } catch (const GenAIException&) {
            throw;
        } catch (const std::exception& e) {
            throw GenAIException(
                GenAIErrorCode::INVALID_REQUEST,
                std::string("Failed to preprocess image ") +
                    std::to_string(i + 1) + ": " + e.what(),
                400);
        }
    }

    return buffers;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────
GenieOrchestrator::GenieOrchestrator() = default;

// ─────────────────────────────────────────────────────────────────────────────
// Step 1: validateRequest
// ─────────────────────────────────────────────────────────────────────────────
void GenieOrchestrator::validateRequest(
    const CreateChatCompletionRequest& request) const {
    auto& config_mgr = ModelConfigManager::getInstance();
    if (!config_mgr.validateModel(request.model)) {
        throw GenAIException(
            GenAIErrorCode::MODEL_NOT_FOUND,
            "Model '" + request.model + "' not found. Check /v1/models for available models.",
            404
        );
    }
    if (request.messages.empty() || !request.messages.is_array()) {
        throw GenAIException(
            GenAIErrorCode::INVALID_REQUEST,
            "messages array is required and must not be empty.",
            400
        );
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// buildContextPrompt — Slot-based prompt assembly
//
// Slot 1: System block (caller's system prompt)
// Slot 1.5: Budget info (reasoning budget notification, if applicable)
// Slot 2: Tool instructions (injected by model adapter)
// Slot 3: Episodic summary (rolling summary of prior history)
// Slot 4: History queue (recent turns, oldest-first)
// Slot 5: Current turn (new user/tool messages)
// ─────────────────────────────────────────────────────────────────────────────
std::string GenieOrchestrator::buildContextPrompt(const ConversationSession& session,
                                                   const CreateChatCompletionRequest& request,
                                                   int thinking_budget,
                                                   int answer_budget,
                                                   bool has_tool_response) const {
    // OIP raw_prompt bypass: when the caller supplied an already-formatted
    // prompt (OIP /generate with text_input), feed it through verbatim
    // without applying the chat template. Safe to return early regardless
    // of thinking_budget/answer_budget — createJob() may call this once for
    // preliminary token estimation (with budgets both 0) and again with the
    // computed budgets; both calls must return the identical fixed string.
    if (request.raw_prompt.has_value()) {
        return request.raw_prompt.value();
    }

    auto& config_mgr = ModelConfigManager::getInstance();
    json chat_template = config_mgr.getChatTemplate(request.model);
    const auto& adapter = ModelAdapterFactory::getAdapter(request.model);

    std::string system_prefix = chat_template.value("system_prefix", "<|system|>\n");
    std::string system_suffix = chat_template.value("system_suffix", "\n");
    std::string user_prefix = chat_template.value("user_prefix", "<|user|>\n");
    std::string user_suffix = chat_template.value("user_suffix", "\n");
    std::string assistant_prefix = chat_template.value("assistant_prefix", "<|assistant|>\n");
    std::string assistant_suffix = chat_template.value("assistant_suffix", "\n");

    std::ostringstream prompt;

    // ── Slot 1: System block (with tool instructions injected by adapter) ──────
    std::string user_system;
    for (const auto& msg : request.messages) {
        if (getStringOrDefault(msg, "role", "") == "system") {
            user_system = messageText(msg);
            break;
        }
    }
    json tools = request.tools.value_or(json::array());
    std::string system_content = adapter.buildSystemPrompt(chat_template, user_system, tools);
    if (!system_content.empty()) {
        prompt << system_prefix << system_content << system_suffix;
    }

    // ── Slot 1.25: Tool-continuation steering ──────────────────────────────────
    // When the current turn contains a role=="tool" message (Trip 2 of tool
    // calling), the standard tool-calling instructions above remain active in
    // the prompt and can cause smaller models to re-emit another bare tool
    // call instead of a final natural-language answer. Steer the model back
    // toward plain-language output unless it genuinely needs another tool call.
    if (!tools.empty() && tools.is_array() && has_tool_response) {
        prompt << system_prefix
               << "[Tool Result Received]: You already have the result of your "
                  "tool call above. Use it to answer the user's question in "
                  "plain natural language now. Do NOT output another JSON tool "
                  "call unless you genuinely need to call a different tool to "
                  "fully answer the question."
               << system_suffix;
    }

    // ── Slot 1.5: Budget info (reasoning budget notification) ─────────────────
    if (thinking_budget > 0 && answer_budget > 0) {
        prompt << system_prefix
               << "[Reasoning Budget]: Use at most " << thinking_budget
               << " tokens for internal thinking and reserve " << answer_budget
               << " tokens for the final answer. Stop thinking when the "
               << "thinking budget is used. Do not mention this budget in "
               << "the final answer."
               << system_suffix;
    }

    // ── Slot 2: Persistent facts (key-value pairs extracted from evicted history)
    std::string facts_text = session.formatFacts();
    if (!facts_text.empty()) {
        prompt << system_prefix << "[Key facts from prior conversation]:\n"
               << facts_text << system_suffix;
    }

    // ── Slot 3: Episodic summary ──────────────────────────────────────────────
    if (!session.summary_content.empty()) {
        prompt << system_prefix << "[Summary of previous conversation]: "
               << session.summary_content << system_suffix;
    }

    // ── Slot 4: History queue (messages not yet evicted) ──────────────────────
    auto clean_messages = session.getHistoryMessages();
    for (const auto& msg : clean_messages) {
        std::string role = getStringOrDefault(msg, "role", "");
        std::string content = messageText(msg);
        if (role == "user") {
            prompt << user_prefix << content << user_suffix;
        } else if (role == "assistant") {
            prompt << assistant_prefix << content;
            if (msg.contains("tool_calls")) {
                prompt << renderToolCallsForPrompt(msg["tool_calls"]);
            }
            prompt << assistant_suffix;
        }
    }

    // ── Slot 5: Current turn ──────────────────────────────────────────────────
    json processed_messages = adapter.preprocessVision(request.messages, chat_template);
    for (const auto& msg : processed_messages) {
        std::string role = getStringOrDefault(msg, "role", "");
        std::string content = messageText(msg);
        if (role == "user") {
            prompt << user_prefix << content << user_suffix;
        } else if (role == "tool") {
            prompt << user_prefix << adapter.formatToolResponse(json::array({msg})) << user_suffix;
        } else if (role == "assistant") {
            prompt << assistant_prefix << content;
            if (msg.contains("tool_calls")) {
                prompt << renderToolCallsForPrompt(msg["tool_calls"]);
            }
            prompt << assistant_suffix;
        }
    }

    // ── Assistant turn start ──────────────────────────────────────────────────
    prompt << assistant_prefix;

    return prompt.str();
}

scheduler::GenerativeJobPtr GenieOrchestrator::createJob(
    scheduler::GenerativeJobContext context,
    scheduler::GenerativeCallbacks callbacks) const {
    CreateChatCompletionRequest request = std::move(context.request);
    request.model = context.model_id;
    validateRequest(request);

    const std::string session_id = context.execution_session_id.empty()
        ? context.job_id : context.execution_session_id;
    ConversationSession session(session_id);
    auto& config_manager = ModelConfigManager::getInstance();
    const bool is_vlm = config_manager.supportsVision(context.model_id);
    const bool uses_private_memory = context.memory_turn.has_value();

    json complete_messages = json::array();
    if (request.messages.is_array()) {
        for (const auto& message : request.messages) {
            if (message.is_object()) {
                complete_messages.push_back(message);
            }
        }
    }

    scheduler::GenieMemoryState input_memory =
        std::move(context.memory_state).value_or(
            scheduler::GenieMemoryState{});

    json post_turn_messages = complete_messages;
    CreateChatCompletionRequest prompt_request = request;
    const bool has_tool_response =
        currentTurnHasToolResponse(complete_messages);
    if (uses_private_memory && !is_vlm) {
        post_turn_messages = canonicalConversationMessages(complete_messages);
        if (!memoryMatchesTranscript(input_memory, post_turn_messages)) {
            LOG_WARN("[GenieOrchestrator] Ignoring incompatible conversation memory: model="
                     << context.model_id << " turn="
                     << context.caller.conversation->turn_id);
            input_memory = scheduler::GenieMemoryState{};
        }
        session.summary_content = input_memory.summary_content;
        session.summary_token_count = input_memory.summary_token_count;
        session.facts = input_memory.facts;
        session.evicted_message_count = input_memory.canonical_boundary;
        for (const auto& message : post_turn_messages) {
            session.addMessage(message);
        }
        prompt_request.messages = json::array();
        for (const auto& message : complete_messages) {
            if (message.value("role", "") == "system") {
                prompt_request.messages.push_back(message);
            }
        }
    }

    const int context_size = config_manager.getContextSize(context.model_id);
    bool use_reasoning =
        !is_vlm && config_manager.supportsThinking(context.model_id);
    int thinking_budget = 0;
    int answer_budget = 0;
    int max_tokens = request.max_completion_tokens.value_or(
        std::min(DEFAULT_MAX_OUTPUT_TOKENS, context_size / 2));

    if (use_reasoning) {
        const std::string preliminary_prompt =
            buildContextPrompt(session, prompt_request, 0, 0, has_tool_response);
        const std::string effort =
            request.reasoning_effort.value_or("medium");
        const ReasoningBudgetResult budget =
            ReasoningBudgetCalculator::compute(
                preliminary_prompt,
                context_size,
                effort,
                request.max_completion_tokens);
        if (budget.context_too_small) {
            throw GenAIException(
                GenAIErrorCode::CONTEXT_LENGTH_EXCEEDED,
                "Context window is too small for this request. Reduce the "
                "conversation history or use a model with a larger context window.",
                400);
        }
        if (budget.suppress_thinking) {
            use_reasoning = false;
        } else {
            thinking_budget = budget.thinking_budget;
            answer_budget = budget.answer_budget;
            max_tokens = budget.thinking_budget + budget.answer_budget;
        }
    }

    scheduler::GeniePreparedRequest prepared;
    prepared.final_prompt = buildContextPrompt(
        session,
        prompt_request,
        thinking_budget,
        answer_budget,
        has_tool_response);
    prepared.generation.max_tokens = max_tokens;
    prepared.generation.temperature = request.temperature.value_or(1.0f);
    prepared.generation.top_p = request.top_p.value_or(1.0f);
    prepared.generation.top_k = request.top_k.value_or(40);
    prepared.generation.presence_penalty =
        request.presence_penalty.value_or(0.0f);
    prepared.generation.frequency_penalty =
        request.frequency_penalty.value_or(0.0f);
    prepared.generation.streaming = static_cast<bool>(callbacks.on_token);
    prepared.generation.use_reasoning = use_reasoning;
    prepared.generation.thinking_budget = thinking_budget;
    const ModelConfig* model_config =
        config_manager.getModelConfig(context.model_id);
    if (model_config) {
        prepared.generation.thinking_start_tag =
            model_config->thinking_start_tag;
        prepared.generation.thinking_end_tag =
            model_config->thinking_end_tag;
    }
    prepared.tools = std::move(request.tools).value_or(json::array());
    prepared.input_memory = std::move(input_memory);
    prepared.uses_private_memory = uses_private_memory;
    if (is_vlm) {
        prepared.vision.buffers =
            preprocessImagesToBuffers(request.messages, context.model_id);
    }
    prepared.conversation_messages = std::move(post_turn_messages);

    auto job = std::make_shared<scheduler::GenerativeJob>();
    job->response_id = context.caller.response_id.empty()
        ? context.job_id : std::move(context.caller.response_id);
    job->session_id = session.session_id;
    job->job_id = std::move(context.job_id);
    job->model_id = std::move(context.model_id);
    job->tool_chain_id = std::move(context.tool_chain_id);
    job->memory_turn = context.memory_turn;
    job->kind = context.kind;
    job->priority = context.priority;
    job->prepared = std::move(prepared);
    job->skip_post_turn_summarization =
        context.skip_post_turn_summarization;
    job->callbacks = std::move(callbacks);
    return job;
}

StandardResponse GenieOrchestrator::execute(
    scheduler::GenerativeJob& job,
    IGenerativeBackend& backend) const {
    const auto* prepared =
        std::get_if<scheduler::GeniePreparedRequest>(&job.prepared);
    if (!prepared) {
        throw std::runtime_error(
            "GenieOrchestrator received a non-Genie prepared request");
    }
    if (prepared->generation.streaming) {
        return executeStreamingPrepared(job, *prepared, backend);
    }
    return executeBlockingPrepared(job, *prepared, backend);
}

StandardResponse GenieOrchestrator::executeBlockingPrepared(
    scheduler::GenerativeJob& job,
    const scheduler::GeniePreparedRequest& prepared,
    IGenerativeBackend& backend) const {
    const bool is_vlm = backend.capabilities().supports_vision;
    const auto& generation = prepared.generation;
    const std::string event_id = generateEventId();
    std::string full_response;
    std::string finish_reason = "stop";
    bool had_error = false;
    std::string error_msg;

    if (is_vlm) {
        LOG_INFO("[GenieOrchestrator] Blocking VLM execution: model="
                 << job.model_id
                 << " session=" << job.session_id
                 << " images=" << prepared.vision.buffers.size());
        backend.generateVlm(
            event_id,
            prepared.final_prompt,
            prepared.vision.buffers,
            false,
            generation.max_tokens,
            generation.temperature,
            generation.top_p,
            generation.top_k,
            generation.presence_penalty,
            generation.frequency_penalty,
            [&full_response](const IPCTokenEvent& token) {
                full_response += token.content;
            },
            [&finish_reason](const IPCDoneEvent& done) {
                finish_reason = done.finish_reason;
            },
            [&had_error, &error_msg](const IPCErrorEvent& err) {
                had_error = true;
                error_msg = err.message;
            });
    } else {
        backend.generate(
            event_id,
            prepared.final_prompt,
            false,
            generation.max_tokens,
            generation.temperature,
            generation.top_p,
            generation.top_k,
            generation.presence_penalty,
            generation.frequency_penalty,
            generation.use_reasoning,
            [&full_response](const IPCTokenEvent& token) {
                full_response += token.content;
            },
            [&finish_reason](const IPCDoneEvent& done) {
                finish_reason = done.finish_reason;
            },
            [&had_error, &error_msg](const IPCErrorEvent& err) {
                had_error = true;
                error_msg = err.message;
            });
    }

    // ── Eager background KV reset ─────────────────────────────────────────────
    backend.resetKvAsync();

    if (had_error) {
        LOG_ERROR("[GenieOrchestrator] Blocking inference failed: model="
                  << job.model_id << " session=" << job.session_id
                  << " message=\"" << error_msg << "\"");
        throw GenAIException(GenAIErrorCode::INFERENCE_FAILED, error_msg, 500);
    }

    if (job.isCancelled()) {
        LOG_WARN("[GenieOrchestrator] Blocking inference cancelled before commit: model="
                 << job.model_id
                 << " session=" << job.session_id);
        return StandardResponse{};
    }

    std::string thinking_content;
    std::string answer_content = full_response;
    int reasoning_token_count = 0;
    if (generation.use_reasoning) {
        ReasoningRouter router(
            job.session_id,
            job.model_id,
            generation.thinking_start_tag,
            generation.thinking_end_tag,
            generation.thinking_budget);
        router.route(full_response);
        thinking_content = router.getThinkingContent();
        answer_content = router.getAnswerContent();
        reasoning_token_count = router.getThinkingTokenCount();
    }

    json tool_calls = json::array();
    if (prepared.tools.is_array() && !prepared.tools.empty()) {
        const auto& adapter = ModelAdapterFactory::getAdapter(job.model_id);
        tool_calls = adapter.parseToolCalls(answer_content);
    }

    if (job.isCancelled()) {
        LOG_WARN("[GenieOrchestrator] Blocking inference cancelled before session update: model="
                 << job.model_id
                 << " session=" << job.session_id);
        return StandardResponse{};
    }

    StandardResponse response;
    response.id = job.response_id;
    response.model = job.model_id;
    response.role = "assistant";
    // Only populate response.content with the raw answer text when no tool
    // calls were parsed out of it — otherwise the structured tool_calls
    // array already carries the extracted call(s), and leaving content set
    // to the same raw text would leak the tool-call JSON (or partial
    // remnants of it) into the OpenAI-format response's message.content.
    if (tool_calls.empty()) {
        response.content = answer_content;
    }
    if (!thinking_content.empty()) {
        response.reasoning_content = thinking_content;
    }
    if (!tool_calls.empty()) {
        response.tool_calls = tool_calls;
    }
    response.finish_reason = tool_calls.empty() ? finish_reason : "tool_calls";
    response.prompt_tokens =
        static_cast<int>(prepared.final_prompt.size() / 4);
    // completion_tokens includes both answer tokens AND reasoning tokens.
    // Non-empty answers always report at least 1 token — otherwise short
    // answers (e.g. "4") truncate to 0 via integer division.
    int answer_token_estimate = answer_content.empty()
        ? 0 : static_cast<int>(answer_content.size() / 4) + 1;
    response.completion_tokens = answer_token_estimate + reasoning_token_count;
    response.reasoning_tokens = reasoning_token_count;
    // total_tokens = input + output (output already includes reasoning)
    response.total_tokens = response.prompt_tokens + response.completion_tokens;
    LOG_INFO("[GenieOrchestrator] Blocking inference completed: model="
             << job.model_id << " session=" << job.session_id
             << " finish_reason=" << response.finish_reason
             << " tool_calls="
             << (response.tool_calls.has_value() ? response.tool_calls.value().size() : 0)
             << " prompt_tokens=" << response.prompt_tokens
             << " completion_tokens=" << response.completion_tokens);
    return response;
}

StandardResponse GenieOrchestrator::executeStreamingPrepared(
    scheduler::GenerativeJob& job,
    const scheduler::GeniePreparedRequest& prepared,
    IGenerativeBackend& backend) const {
    const bool is_vlm = backend.capabilities().supports_vision;
    const auto& generation = prepared.generation;
    auto& callback = job.callbacks.on_token;

    StreamChunk role_chunk;
    role_chunk.id = job.response_id;
    role_chunk.model = job.model_id;
    role_chunk.role = "assistant";
    callback(role_chunk);

    ReasoningRouter router(
        job.session_id,
        job.model_id,
        generation.thinking_start_tag,
        generation.thinking_end_tag,
        generation.thinking_budget);

    const std::string event_id = generateEventId();
    std::string full_response;
    std::string finish_reason = "stop";
    bool had_error = false;
    std::string error_msg;

    if (is_vlm) {
        LOG_INFO("[GenieOrchestrator] Streaming VLM execution: model="
                 << job.model_id
                 << " session=" << job.session_id
                 << " images=" << prepared.vision.buffers.size());
        backend.generateVlm(
            event_id,
            prepared.final_prompt,
            prepared.vision.buffers,
            true,
            generation.max_tokens,
            generation.temperature,
            generation.top_p,
            generation.top_k,
            generation.presence_penalty,
            generation.frequency_penalty,
            [&job, &callback, &full_response]
            (const IPCTokenEvent& token) {
                StreamChunk chunk;
                chunk.id = job.response_id;
                chunk.model = job.model_id;
                chunk.content_delta = token.content;
                full_response += token.content;
                callback(chunk);
            },
            [&finish_reason](const IPCDoneEvent& done) {
                finish_reason = done.finish_reason;
            },
            [&had_error, &error_msg](const IPCErrorEvent& err) {
                had_error = true;
                error_msg = err.message;
            });
    } else {
        backend.generate(
            event_id,
            prepared.final_prompt,
            true,
            generation.max_tokens,
            generation.temperature,
            generation.top_p,
            generation.top_k,
            generation.presence_penalty,
            generation.frequency_penalty,
            generation.use_reasoning,
            [&generation, &router, &job, &callback, &full_response]
            (const IPCTokenEvent& token) {
                if (generation.use_reasoning) {
                    auto chunks = router.route(token.content);
                    for (auto& chunk : chunks) {
                        chunk.id = job.response_id;
                        chunk.model = job.model_id;
                        callback(chunk);
                        if (chunk.content_delta.has_value()) {
                            full_response += chunk.content_delta.value();
                        }
                    }
                    return;
                }

                StreamChunk chunk;
                chunk.id = job.response_id;
                chunk.model = job.model_id;
                chunk.content_delta = token.content;
                full_response += token.content;
                callback(chunk);
            },
            [&finish_reason](const IPCDoneEvent& done) {
                finish_reason = done.finish_reason;
            },
            [&had_error, &error_msg](const IPCErrorEvent& err) {
                had_error = true;
                error_msg = err.message;
            });
    }

    // ── Eager background KV reset ─────────────────────────────────────────────
    backend.resetKvAsync();

    if (had_error) {
        LOG_ERROR("[GenieOrchestrator] Streaming inference failed: model="
                  << job.model_id << " session=" << job.session_id
                  << " message=\"" << error_msg << "\"");
        throw GenAIException(GenAIErrorCode::INFERENCE_FAILED, error_msg, 500);
    }

    if (job.isCancelled()) {
        LOG_WARN("[GenieOrchestrator] Streaming inference cancelled before commit: model="
                 << job.model_id
                 << " session=" << job.session_id);
        return StandardResponse{};
    }

    std::string thinking_content;
    std::string answer_content = full_response;
    int reasoning_token_count = 0;
    if (generation.use_reasoning) {
        thinking_content = router.getThinkingContent();
        answer_content = router.getAnswerContent();
        reasoning_token_count = router.getThinkingTokenCount();
    }

    json tool_calls = json::array();
    if (prepared.tools.is_array() && !prepared.tools.empty()) {
        const auto& adapter = ModelAdapterFactory::getAdapter(job.model_id);
        tool_calls = adapter.parseToolCalls(answer_content);
    }

    StreamChunk finish_chunk;
    finish_chunk.id = job.response_id;
    finish_chunk.model = job.model_id;
    finish_chunk.finish_reason = tool_calls.empty() ? finish_reason : "tool_calls";
    callback(finish_chunk);

    StandardResponse response;
    response.id = job.response_id;
    response.model = job.model_id;
    response.role = "assistant";
    // Only populate response.content with the raw answer text when no tool
    // calls were parsed out of it — otherwise the structured tool_calls
    // array already carries the extracted call(s), and leaving content set
    // to the same raw text would leak the tool-call JSON (or partial
    // remnants of it) into the OpenAI-format response's message.content.
    if (tool_calls.empty()) {
        response.content = answer_content;
    }
    if (!thinking_content.empty()) {
        response.reasoning_content = thinking_content;
    }
    if (!tool_calls.empty()) {
        response.tool_calls = tool_calls;
    }
    response.finish_reason = tool_calls.empty() ? finish_reason : "tool_calls";
    response.prompt_tokens =
        static_cast<int>(prepared.final_prompt.size() / 4);
    // completion_tokens includes both answer tokens AND reasoning tokens.
    // Non-empty answers always report at least 1 token — otherwise short
    // answers (e.g. "4") truncate to 0 via integer division.
    int answer_token_estimate = answer_content.empty()
        ? 0 : static_cast<int>(answer_content.size() / 4) + 1;
    response.completion_tokens = answer_token_estimate + reasoning_token_count;
    response.reasoning_tokens = reasoning_token_count;
    // total_tokens = input + output (output already includes reasoning)
    response.total_tokens = response.prompt_tokens + response.completion_tokens;
    LOG_INFO("[GenieOrchestrator] Streaming inference completed: model="
             << job.model_id << " session=" << job.session_id
             << " finish_reason=" << response.finish_reason
             << " response_chars=" << answer_content.size());
    return response;
}

std::optional<scheduler::PostTurnTask>
GenieOrchestrator::createPostTurnTask(
    scheduler::GenerativeJob& job,
    const StandardResponse& response) const {
    auto* prepared =
        std::get_if<scheduler::GeniePreparedRequest>(&job.prepared);
    if (!prepared || !job.memory_turn.has_value() ||
        job.skip_post_turn_summarization ||
        !prepared->vision.buffers.empty() ||
        (response.tool_calls.has_value() &&
         !response.tool_calls.value().empty())) {
        return std::nullopt;
    }

    scheduler::PostTurnTask task;
    task.input.session_id = job.session_id;
    task.input.input_memory = std::move(prepared->input_memory);
    task.input.memory_turn = job.memory_turn.value();
    task.input.request_messages =
        std::move(prepared->conversation_messages);
    task.response = response;
    return task;
}

scheduler::GenieMemoryState GenieOrchestrator::executePostTurn(
    scheduler::PostTurnTask& task,
    IGenerativeBackend& backend) const {
    ConversationSession session(task.input.session_id);
    session.summary_content = task.input.input_memory.summary_content;
    session.summary_token_count = task.input.input_memory.summary_token_count;
    session.facts = task.input.input_memory.facts;
    session.evicted_message_count =
        task.input.input_memory.canonical_boundary;

    if (task.input.request_messages.is_array()) {
        for (auto& message : task.input.request_messages) {
            if (message.is_object()) {
                session.addMessage(std::move(message));
            }
        }
    }

    json assistant_message = {
        {"role", "assistant"},
        {"content", task.response.content.value_or("")},
    };
    if (task.response.reasoning_content.has_value() &&
        !task.response.reasoning_content.value().empty()) {
        assistant_message["_thinking_content"] =
            task.response.reasoning_content.value();
    }
    session.addMessage(assistant_message);

    postTurnProcessing(session, task.response.model, backend);
    scheduler::GenieMemoryState updated;
    updated.summary_content = session.summary_content;
    updated.summary_token_count = session.summary_token_count;
    updated.facts = session.facts;
    updated.canonical_boundary = session.evicted_message_count;
    if (updated.canonical_boundary != 0) {
        json canonical = json::array();
        for (const auto& message : session.messages) {
            canonical.push_back(message);
        }
        updated.transcript_digest = canonicalPrefixDigest(
            canonical, updated.canonical_boundary);
    }
    return updated;
}

// ─────────────────────────────────────────────────────────────────────────────
// Post-turn memory management
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Simple token estimator: 1 token ≈ 4 characters.
static int estimateTokens(const std::string& text) {
    return static_cast<int>(text.size() / 4) + 1;
}

// Build a chat-template-formatted prompt for a single user message.
static std::string buildSingleTurnPrompt(
    const std::string& user_content,
    const std::string& model_id)
{
    auto& config_mgr = ModelConfigManager::getInstance();
    json chat_template = config_mgr.getChatTemplate(model_id);

    std::string system_prefix    = chat_template.value("system_prefix",    "<|system|>\n");
    std::string system_suffix    = chat_template.value("system_suffix",    "\n");
    std::string user_prefix      = chat_template.value("user_prefix",      "<|user|>\n");
    std::string user_suffix      = chat_template.value("user_suffix",      "\n");
    std::string assistant_prefix = chat_template.value("assistant_prefix", "<|assistant|>\n");

    std::ostringstream oss;
    oss << system_prefix
        << "You are a helpful assistant."
        << system_suffix
        << user_prefix
        << user_content
        << user_suffix
        << assistant_prefix;
    return oss.str();
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// generateSummary
// ─────────────────────────────────────────────────────────────────────────────
std::pair<std::string, int>
GenieOrchestrator::generateSummary(
    const ConversationSession& session,
    const std::vector<json>& messages_to_summarize,
    const std::string& model_id,
    int max_summary_tokens,
    IGenerativeBackend& backend) const
{
    // Build conversation text from the eviction batch.
    // Exclude tool messages (verbose, rarely useful in summaries).
    std::ostringstream conv;
    int input_tokens = 0;
    const int input_budget = 2048;  // cap input to avoid blowing context

    // Prepend prior summary for continuity (rolling summary).
    if (!session.summary_content.empty()) {
        conv << "[Prior summary]: " << session.summary_content << "\n\n";
        input_tokens += estimateTokens(session.summary_content);
    }

    for (const auto& msg : messages_to_summarize) {
        std::string role = getStringOrDefault(msg, "role", "");
        if (role == "tool" || role == "system") continue;

        std::string content = messageText(msg);
        if (content.empty()) continue;

        int msg_tokens = estimateTokens(content);
        if (input_tokens + msg_tokens > input_budget) break;

        std::string role_label = (role == "user") ? "User" : "Assistant";
        conv << role_label << ": " << content << "\n";
        input_tokens += msg_tokens;
    }

    std::string conversation_text = conv.str();
    if (conversation_text.empty()) return {"", 0};

    // Build summarization prompt.
    std::ostringstream user_msg;
    user_msg << "Summarize the following conversation in a concise, structured format. "
             << "Preserve key facts, decisions made, tasks completed, open items, "
             << "and important context. Keep the summary under "
             << max_summary_tokens << " tokens.\n\n"
             << "Conversation:\n"
             << conversation_text;

    std::string prompt = buildSingleTurnPrompt(user_msg.str(), model_id);

    // Run summarization inference.
    std::string summary_text;
    bool had_error = false;
    std::string error_msg;

    backend.generate(
        generateEventId() + "-summary",
        prompt,
        false,
        max_summary_tokens,
        0.3f,   // low temperature for factual summary
        1.0f,
        40,
        0.0f,
        0.0f,
        false,  // no reasoning
        [&summary_text](const IPCTokenEvent& token) {
            summary_text += token.content;
        },
        [](const IPCDoneEvent&) {},
        [&had_error, &error_msg](const IPCErrorEvent& err) {
            had_error = true;
            error_msg = err.message;
        });

    if (had_error) {
        throw std::runtime_error("Summary inference failed: " + error_msg);
    }

    // Strip leading/trailing whitespace.
    auto trim = [](std::string s) {
        s.erase(0, s.find_first_not_of(" \t\n\r"));
        s.erase(s.find_last_not_of(" \t\n\r") + 1);
        return s;
    };
    summary_text = trim(summary_text);

    int summary_tokens = estimateTokens(summary_text);
    LOG_INFO("[GenieOrchestrator] generateSummary: "
             << messages_to_summarize.size() << " messages → "
             << summary_tokens << " tokens");
    return {summary_text, summary_tokens};
}

// ─────────────────────────────────────────────────────────────────────────────
// extractFacts
// ─────────────────────────────────────────────────────────────────────────────
void GenieOrchestrator::extractFacts(
    ConversationSession& session,
    const std::vector<json>& messages_to_extract,
    const std::string& model_id,
    IGenerativeBackend& backend) const
{
    // Build conversation text.
    std::ostringstream conv;
    for (const auto& msg : messages_to_extract) {
        std::string role = getStringOrDefault(msg, "role", "");
        if (role != "user" && role != "assistant") continue;
        std::string content = messageText(msg);
        if (content.empty()) continue;
        std::string label = (role == "user") ? "User" : "Assistant";
        conv << label << ": " << content << "\n";
    }

    std::string conversation_text = conv.str();
    if (conversation_text.empty()) return;

    // Build fact extraction prompt.
    std::ostringstream user_msg;
    user_msg << "Extract the most important persistent facts from this conversation "
             << "as a JSON object. Facts should be short key-value pairs "
             << "(names, goals, decisions, constraints, preferences). "
             << "Return ONLY a valid JSON object like: "
             << "{\"name\": \"Alice\", \"goal\": \"build a chatbot\"}. "
             << "Limit to the most important facts (max 10).\n\n"
             << "Conversation:\n"
             << conversation_text;

    std::string prompt = buildSingleTurnPrompt(user_msg.str(), model_id);

    // Run fact extraction inference.
    std::string raw_response;
    bool had_error = false;
    std::string error_msg;

    backend.generate(
        generateEventId() + "-facts",
        prompt,
        false,
        150,    // max 150 tokens for facts JSON
        0.0f,   // zero temperature for deterministic extraction
        1.0f,
        40,
        0.0f,
        0.0f,
        false,
        [&raw_response](const IPCTokenEvent& token) {
            raw_response += token.content;
        },
        [](const IPCDoneEvent&) {},
        [&had_error, &error_msg](const IPCErrorEvent& err) {
            had_error = true;
            error_msg = err.message;
        });

    if (had_error) {
        LOG_WARN("[GenieOrchestrator] extractFacts inference failed: " << error_msg);
        return;
    }

    // Parse JSON from the response (model may add surrounding text).
    std::unordered_map<std::string, std::string> new_facts;
    try {
        // Try direct parse first.
        json parsed = json::parse(raw_response);
        if (parsed.is_object()) {
            for (const auto& [k, v] : parsed.items()) {
                if (v.is_string()) {
                    new_facts[k] = v.get<std::string>();
                } else {
                    new_facts[k] = v.dump();
                }
            }
        }
    } catch (...) {
        // Try to find a JSON object in the response.
        auto start = raw_response.find('{');
        auto end   = raw_response.rfind('}');
        if (start != std::string::npos && end != std::string::npos && end > start) {
            try {
                json parsed = json::parse(raw_response.substr(start, end - start + 1));
                if (parsed.is_object()) {
                    for (const auto& [k, v] : parsed.items()) {
                        if (v.is_string()) {
                            new_facts[k] = v.get<std::string>();
                        } else {
                            new_facts[k] = v.dump();
                        }
                    }
                }
            } catch (...) {
                // Could not parse facts — skip silently.
            }
        }
    }

    if (!new_facts.empty()) {
        session.updateFacts(new_facts);
        session.enforceFactsCeiling(20);
        LOG_INFO("[GenieOrchestrator] extractFacts: extracted "
                 << new_facts.size() << " facts ("
                 << session.facts.size() << " total)");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// postTurnProcessing
// ─────────────────────────────────────────────────────────────────────────────
void GenieOrchestrator::postTurnProcessing(
    ConversationSession& session,
    const std::string& model_id,
    IGenerativeBackend& backend) const
{
    // Thresholds (matching Python service).
    constexpr float kHistoryFraction      = 0.40f;  // 40% of context for history
    constexpr float kEvictionThreshold    = 0.80f;  // evict when > 80% of budget
    constexpr float kEvictionTarget       = 0.60f;  // evict down to 60% of budget
    constexpr float kSummarySizeRatio     = 0.20f;  // summary = 20% of input budget
    constexpr int   kSafetyMargin         = 64;

    auto& config_mgr = ModelConfigManager::getInstance();
    int context_size = config_mgr.getContextSize(model_id);

    // Compute history budget.
    int output_reserve  = context_size / 2;
    int input_budget    = context_size - output_reserve - kSafetyMargin;
    int history_budget  = static_cast<int>(input_budget * kHistoryFraction);
    int eviction_thresh = static_cast<int>(history_budget * kEvictionThreshold);
    int eviction_target = static_cast<int>(history_budget * kEvictionTarget);
    int max_summary_tok = static_cast<int>(input_budget * kSummarySizeRatio);
    max_summary_tok     = std::min(max_summary_tok, 400);

    // Estimate current history tokens (messages after evicted_message_count).
    int current_tokens = 0;
    for (size_t message_index = session.evicted_message_count;
         message_index < session.messages.size();
         ++message_index) {
        const auto& message = session.messages[message_index];
        if (message.value("role", "") == "system") {
            continue;
        }
        current_tokens += static_cast<int>(messageText(message).size() / 4) + 4;
        if (message.contains("tool_calls")) {
            current_tokens += static_cast<int>(
                message["tool_calls"].dump().size() / 4);
        }
    }

    if (current_tokens <= eviction_thresh) {
        LOG_DEBUG("[GenieOrchestrator] postTurnProcessing: no eviction needed ("
                  << current_tokens << "/" << eviction_thresh << " tokens)");
        return;
    }

    // Identify eviction batch: remove oldest messages until under target.
    std::vector<json> eviction_batch;
    size_t evict_count = 0;
    int remaining_tokens = current_tokens;

    size_t index = session.evicted_message_count;
    while (index < session.messages.size() &&
           remaining_tokens > eviction_target) {
        size_t group_end = index + 1;
        const auto& first = session.messages[index];
        const bool opens_tool_group =
            first.value("role", "") == "assistant" &&
            first.contains("tool_calls") && first["tool_calls"].is_array() &&
            !first["tool_calls"].empty();
        if (opens_tool_group) {
            while (group_end < session.messages.size() &&
                   session.messages[group_end].value("role", "") == "tool") {
                ++group_end;
            }
        }

        for (size_t message_index = index;
             message_index < group_end;
             ++message_index) {
            const auto& message = session.messages[message_index];
            ++evict_count;
            if (message.value("role", "") == "system") {
                continue;
            }
            eviction_batch.push_back(message);
            const std::string content = messageText(message);
            remaining_tokens -= static_cast<int>(content.size() / 4) + 4;
            if (message.contains("tool_calls")) {
                remaining_tokens -= static_cast<int>(
                    message["tool_calls"].dump().size() / 4);
            }
        }
        index = group_end;
    }

    if (eviction_batch.empty()) return;

    LOG_INFO("[GenieOrchestrator] postTurnProcessing: evicting "
             << evict_count << " messages (history="
             << current_tokens << " tokens, target="
             << eviction_target << ")");

    // ── Step 1: Summarize eviction batch ─────────────────────────────────────
    // The KV cache was reset by resetKvAsync() before this call.
    // generateSummary() calls backend.generate() which calls waitForPendingReset()
    // internally, ensuring the KV is clean before the summarization inference.
    try {
        auto [summary_text, summary_tokens] = generateSummary(
            session, eviction_batch, model_id, max_summary_tok, backend);
        if (summary_text.empty()) {
            throw std::runtime_error(
                "Post-turn summarization produced an empty result");
        }
        session.summary_content = summary_text;
        session.summary_token_count = summary_tokens;
        LOG_INFO("[GenieOrchestrator] postTurnProcessing: summary updated ("
                 << summary_tokens << " tokens)");
    } catch (const std::exception& e) {
        LOG_WARN("[GenieOrchestrator] postTurnProcessing: summarization failed: "
                 << e.what());
        backend.resetKvAsync();
        throw;
    }

    // Reset KV cache after summarization inference.
    backend.resetKvAsync();

    // ── Step 2: Extract facts from eviction batch ─────────────────────────────
    // extractFacts() calls backend.generate() which calls waitForPendingReset()
    // internally, ensuring the KV is clean before the fact extraction inference.
    try {
        extractFacts(session, eviction_batch, model_id, backend);
    } catch (const std::exception& e) {
        LOG_WARN("[GenieOrchestrator] postTurnProcessing: fact extraction failed: "
                 << e.what());
        backend.resetKvAsync();
        throw;
    }

    // Reset KV cache after fact extraction inference.
    backend.resetKvAsync();

    // ── Step 3: Advance eviction pointer ─────────────────────────────────────
    // Messages [evicted_message_count, evicted_message_count + evict_count)
    // are now represented by summary_content and facts.
    // The next turn's getHistoryMessages() will exclude them from Slot 4.
    session.evicted_message_count += evict_count;

    LOG_INFO("[GenieOrchestrator] postTurnProcessing complete: "
             << "evicted=" << evict_count
             << " total_evicted=" << session.evicted_message_count
             << " facts=" << session.facts.size());
}
