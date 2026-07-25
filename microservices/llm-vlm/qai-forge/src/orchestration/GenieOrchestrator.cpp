// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// GenieOrchestrator — Genie-specific IOrchestrator Implementation
//
// Implements IOrchestrator for the Qualcomm GenIE SDK backend.
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
#include "qai_forge/scheduler/InferenceJob.h"
#include "qai_forge/utils/ImageUtils.h"
#include "qai_forge/utils/Logger.h"
#include <algorithm>
#include <exception>
#include <regex>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <random>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr int DEFAULT_MAX_OUTPUT_TOKENS    = 512;
constexpr int MAX_COMPLETION_SAFETY_MARGIN = 64;

bool isCancellationRequested(
    const GenieOrchestrator::CancellationPredicate& cancel_requested) {
    return cancel_requested && cancel_requested();
}

bool requestHasTools(const CreateChatCompletionRequest& request) {
    if (!request.tools.has_value() || !request.tools.value().is_array()) {
        return false;
    }
    return !request.tools.value().empty();
}

std::string generateEventId() {
    static std::mt19937_64 rng(std::random_device{}());
    std::ostringstream oss;
    oss << "evt-" << std::hex << rng();
    return oss.str();
}

void ensureWorkerRunning(const CreateChatCompletionRequest& request,
                         IGenerativeBackend& backend) {
    const ModelConfig* model_config =
        ModelConfigManager::getInstance().getModelConfig(request.model);
    backend.ensureWorkerRunning(
        request.model,
        model_config ? model_config->config_file : "",
        model_config ? model_config->sampler_config_file : "");
}

void registerSessionHash(const CreateChatCompletionRequest& request,
                         const std::string& session_id) {
    auto& session_mgr = SessionManager::getInstance();
    std::string hash = SessionManager::calculateMessagesHash(request.messages);
    session_mgr.registerHash(hash, session_id);
}

ConversationMemoryUpdate makeConversationMemoryUpdate(
    const ConversationSession& session) {
    ConversationMemoryUpdate update;
    update.summary_content = session.summary_content;
    update.summary_token_count = session.summary_token_count;
    update.facts = session.facts;
    update.evicted_message_count = session.evicted_message_count;
    return update;
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

ImageUtils::TempFileGuard preprocessImagesToTempFiles(
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

    ImageUtils::TempFileGuard guard;
    for (size_t i = 0; i < image_urls.size(); ++i) {
        try {
            std::string temp_path = ImageUtils::preprocessImageToTempFile(
                image_urls[i],
                vision_cfg,
                model_id);
            guard.paths.push_back(temp_path);
            LOG_INFO("[GenieOrchestrator] Preprocessed VLM image "
                     << (i + 1) << "/" << image_urls.size()
                     << ": model=" << model_id
                     << " path=" << temp_path);
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

    return guard;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────
GenieOrchestrator::GenieOrchestrator() = default;

GenieOrchestrator& GenieOrchestrator::getInstance() {
    static GenieOrchestrator instance;
    return instance;
}

// ─────────────────────────────────────────────────────────────────────────────
// Step 1: validateRequest
// ─────────────────────────────────────────────────────────────────────────────
void GenieOrchestrator::validateRequest(const CreateChatCompletionRequest& request) {
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
// Step 2: resolveSessionAndDraft
// ─────────────────────────────────────────────────────────────────────────────
std::pair<std::shared_ptr<ConversationSession>, DraftTurn>
GenieOrchestrator::resolveSessionAndDraft(const CreateChatCompletionRequest& request) {
    auto& session_mgr = SessionManager::getInstance();

    std::string session_id;
    if (request.user.has_value() && !request.user.value().empty()) {
        session_id = request.user.value();
    } else {
        std::string hash = SessionManager::calculateMessagesHash(request.messages);
        auto existing = session_mgr.findByHash(hash);
        if (existing) {
            session_id = existing->session_id;
        } else {
            session_id = SessionManager::generateSessionId();
        }
    }

    auto [session, is_new] = session_mgr.findOrCreate(session_id, request.user.value_or("default_user"));

    DraftTurn draft(session_id, request.model);

    if (is_new) {
        for (const auto& msg : request.messages) {
            draft.addMessage(msg);
        }
    } else {
        size_t existing_count = session->messages.size();
        size_t incoming_count = request.messages.size();
        if (incoming_count > existing_count) {
            for (size_t i = existing_count; i < incoming_count; ++i) {
                draft.addMessage(request.messages[i]);
            }
        }
    }

    return {session, std::move(draft)};
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
                                                   int answer_budget) const {
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
        if (msg.value("role", "") == "system") {
            user_system = msg.value("content", "");
            break;
        }
    }
    json tools = request.tools.value_or(json::array());
    std::string system_content = adapter.buildSystemPrompt(chat_template, user_system, tools);
    if (!system_content.empty()) {
        prompt << system_prefix << system_content << system_suffix;
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
        std::string role = msg.value("role", "");
        std::string content = msg.value("content", "");
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
    json processed_messages = adapter.preprocessVision(request.messages);
    for (const auto& msg : processed_messages) {
        std::string role = msg.value("role", "");
        std::string content = msg.value("content", "");
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

StandardResponse GenieOrchestrator::executeBlocking(
    const CreateChatCompletionRequest& request,
    IGenerativeBackend& backend,
    CancellationPredicate cancel_requested,
    bool skip_summarization_middleware) {
    validateRequest(request);
    auto [session, draft] = resolveSessionAndDraft(request);
    LOG_INFO("[GenieOrchestrator] Prepared blocking request: model="
             << request.model << " session=" << session->session_id
             << " stream=" << (request.stream ? "true" : "false")
             << " message_count=" << request.messages.size()
             << " has_tools=" << (request.tools.has_value() ? "true" : "false"));
    return executeBlockingPrepared(
        request,
        std::move(session),
        std::move(draft),
        backend,
        cancel_requested,
        skip_summarization_middleware,
        true);
}

StandardResponse GenieOrchestrator::executeStreaming(
    const CreateChatCompletionRequest& request,
    IGenerativeBackend& backend,
    OrchestratorStreamCallback callback,
    CancellationPredicate cancel_requested,
    bool skip_summarization_middleware) {
    validateRequest(request);
    auto [session, draft] = resolveSessionAndDraft(request);
    LOG_INFO("[GenieOrchestrator] Prepared streaming request: model="
             << request.model << " session=" << session->session_id
             << " stream=" << (request.stream ? "true" : "false")
             << " message_count=" << request.messages.size()
             << " has_tools=" << (request.tools.has_value() ? "true" : "false"));
    return executeStreamingPrepared(
        request,
        std::move(session),
        std::move(draft),
        backend,
        std::move(callback),
        cancel_requested,
        skip_summarization_middleware,
        true);
}

// ─────────────────────────────────────────────────────────────────────────────
// execute() — IOrchestrator implementation
//
// Primary entry point for the scheduler path (ModelRuntime).
// Creates a transient ConversationSession seeded from response_history.
// Blocking when callback is nullptr; streaming otherwise.
// ─────────────────────────────────────────────────────────────────────────────
StandardResponse GenieOrchestrator::execute(
    const CreateChatCompletionRequest& request,
    const scheduler::SchedulerInvokeOptions& options,
    IGenerativeBackend& backend,
    OrchestratorStreamCallback callback,
    std::function<bool()> cancel) {
    validateRequest(request);
    const std::string session_id = request.user.value_or("");
    auto session = std::make_shared<ConversationSession>(
        session_id.empty() ? request.model : session_id);
    session->summary_content = options.summary_content;
    session->summary_token_count = options.summary_token_count;
    session->facts = options.facts;
    session->evicted_message_count = options.evicted_message_count;

    static const json kEmptyResponseHistory = json::array();
    const json& response_history = options.use_response_history
        ? options.response_history
        : kEmptyResponseHistory;
    if (response_history.is_array()) {
        for (const auto& message : response_history) {
            if (message.is_object()) {
                session->addMessage(message);
            }
        }
    }

    DraftTurn draft(session->session_id, request.model);
    for (const auto& message : request.messages) {
        if (message.is_object()) {
            draft.addMessage(message);
        }
    }

    if (callback) {
        return executeStreamingPrepared(
            request,
            std::move(session),
            std::move(draft),
            backend,
            std::move(callback),
            cancel,
            /*skip_summarization_middleware=*/true,
            /*register_session_hash=*/false);
    } else {
        return executeBlockingPrepared(
            request,
            std::move(session),
            std::move(draft),
            backend,
            cancel,
            /*skip_summarization_middleware=*/true,
            /*register_session_hash=*/false);
    }
}

StandardResponse GenieOrchestrator::executeBlockingPrepared(
    const CreateChatCompletionRequest& request,
    std::shared_ptr<ConversationSession> session,
    DraftTurn&& draft,
    IGenerativeBackend& backend,
    const CancellationPredicate& cancel_requested,
    bool /*skip_summarization_middleware*/,
    bool register_session_hash) {
    auto& config_mgr = ModelConfigManager::getInstance();
    const bool is_vlm = config_mgr.supportsVision(request.model);
    int context_size = config_mgr.getContextSize(request.model);

    // ── Calculate reasoning budget BEFORE building prompt ─────────────────────
    bool use_reasoning = !is_vlm && config_mgr.supportsThinking(request.model);
    int thinking_budget = 0;
    int answer_budget = 0;
    int effective_max_tokens = request.max_completion_tokens.value_or(
        std::min(DEFAULT_MAX_OUTPUT_TOKENS, context_size / 2));

    if (use_reasoning) {
        // Build preliminary prompt to estimate input tokens
        std::string prelim_prompt = buildContextPrompt(*session, request);
        std::string effort = request.reasoning_effort.value_or("medium");
        ReasoningBudgetResult budget = ReasoningBudgetCalculator::compute(
            prelim_prompt,
            config_mgr.getContextSize(request.model),
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
            LOG_INFO("[GenieOrchestrator] Thinking suppressed for model "
                     << request.model << " (effort='" << effort
                     << "', budget=" << budget.thinking_budget << ")");
        } else {
            thinking_budget = budget.thinking_budget;
            answer_budget = budget.answer_budget;
            effective_max_tokens = budget.thinking_budget + budget.answer_budget;
            LOG_INFO("[GenieOrchestrator] Reasoning budget for model "
                     << request.model << ": effort='" << effort
                     << "' thinking=" << thinking_budget
                     << " answer=" << answer_budget
                     << " input_est=" << budget.estimated_input_tokens);
        }
    }

    // ── Build final prompt with budget injection ──────────────────────────────
    std::string prompt = buildContextPrompt(*session, request, thinking_budget, answer_budget);
    LOG_INFO("[GenieOrchestrator] Blocking prompt built: model="
             << request.model << " session=" << session->session_id
             << " prompt_chars=" << prompt.size());

    ensureWorkerRunning(request, backend);
    LOG_INFO("[GenieOrchestrator] Blocking backend ready: model="
             << request.model << " session=" << session->session_id
             << " max_tokens=" << effective_max_tokens
             << " reasoning=" << (use_reasoning ? "true" : "false"));

    std::string event_id = generateEventId();
    std::string full_response;
    std::string finish_reason = "stop";
    bool had_error = false;
    std::string error_msg;

    if (is_vlm) {
        ImageUtils::TempFileGuard img_guard =
            preprocessImagesToTempFiles(request.messages, request.model);
        LOG_INFO("[GenieOrchestrator] Blocking VLM execution: model="
                 << request.model
                 << " session=" << session->session_id
                 << " images=" << img_guard.paths.size());
        backend.generateVlm(
            event_id,
            prompt,
            img_guard.paths,
            false,
            effective_max_tokens,
            request.temperature.value_or(1.0f),
            request.top_p.value_or(1.0f),
            request.top_k.value_or(40),
            request.presence_penalty.value_or(0.0f),
            request.frequency_penalty.value_or(0.0f),
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
            prompt,
            false,
            effective_max_tokens,
            request.temperature.value_or(1.0f),
            request.top_p.value_or(1.0f),
            request.top_k.value_or(40),
            request.presence_penalty.value_or(0.0f),
            request.frequency_penalty.value_or(0.0f),
            use_reasoning,
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
                  << request.model << " session=" << session->session_id
                  << " message=\"" << error_msg << "\"");
        throw GenAIException(GenAIErrorCode::INFERENCE_FAILED, error_msg, 500);
    }

    if (isCancellationRequested(cancel_requested)) {
        LOG_WARN("[GenieOrchestrator] Blocking inference cancelled before commit: model="
                 << request.model
                 << " session=" << session->session_id);
        return StandardResponse{};
    }

    std::string thinking_content;
    std::string answer_content = full_response;
    int reasoning_token_count = 0;
    if (use_reasoning) {
        const ModelConfig* model_config =
            config_mgr.getModelConfig(request.model);
        std::string start_tag =
            model_config ? model_config->thinking_start_tag : "<think>";
        std::string end_tag =
            model_config ? model_config->thinking_end_tag : "</think>";
        ReasoningRouter router(
            session->session_id,
            request.model,
            start_tag,
            end_tag,
            thinking_budget);
        router.route(full_response);
        thinking_content = router.getThinkingContent();
        answer_content = router.getAnswerContent();
        reasoning_token_count = router.getThinkingTokenCount();
    }

    json tool_calls = json::array();
    if (requestHasTools(request)) {
        const auto& adapter = ModelAdapterFactory::getAdapter(request.model);
        tool_calls = adapter.parseToolCalls(answer_content);
    }

    if (isCancellationRequested(cancel_requested)) {
        LOG_WARN("[GenieOrchestrator] Blocking inference cancelled before session update: model="
                 << request.model
                 << " session=" << session->session_id);
        return StandardResponse{};
    }

    draft.commit(*session);
    json assistant_msg = {{"role", "assistant"}, {"content", answer_content}};
    if (!thinking_content.empty()) {
        assistant_msg["_thinking_content"] = thinking_content;
    }
    session->addMessage(assistant_msg);

    // ── Post-turn memory management (LLM only, complete turns only) ───────────
    // Runs synchronously before returning so the next turn's prompt is fully
    // prepared (summary + facts updated) before the response is returned.
    // Skipped for VLM (no KV save/restore) and tool-call turns (turn not done).
    std::optional<ConversationMemoryUpdate> updated_conversation_memory;
    if (!is_vlm && tool_calls.empty()) {
        try {
            postTurnProcessing(*session, request.model, backend);
            updated_conversation_memory = makeConversationMemoryUpdate(*session);
        } catch (const std::exception& e) {
            LOG_WARN("[GenieOrchestrator] Post-turn processing failed (non-fatal): "
                     << e.what());
        }
    }

    if (register_session_hash) {
        registerSessionHash(request, session->session_id);
    }

    StandardResponse response;
    response.id = session->session_id;
    response.model = request.model;
    response.role = "assistant";
    response.content = answer_content;
    if (!thinking_content.empty()) {
        response.reasoning_content = thinking_content;
    }
    if (!tool_calls.empty()) {
        response.tool_calls = tool_calls;
    }
    response.finish_reason = tool_calls.empty() ? finish_reason : "tool_calls";
    response.prompt_tokens = static_cast<int>(prompt.size() / 4);
    // completion_tokens includes both answer tokens AND reasoning tokens.
    // Non-empty answers always report at least 1 token — otherwise short
    // answers (e.g. "4") truncate to 0 via integer division.
    int answer_token_estimate = answer_content.empty()
        ? 0 : static_cast<int>(answer_content.size() / 4) + 1;
    response.completion_tokens = answer_token_estimate + reasoning_token_count;
    response.reasoning_tokens = reasoning_token_count;
    // total_tokens = input + output (output already includes reasoning)
    response.total_tokens = response.prompt_tokens + response.completion_tokens;
    response.updated_conversation_memory = std::move(updated_conversation_memory);
    LOG_INFO("[GenieOrchestrator] Blocking inference completed: model="
             << request.model << " session=" << session->session_id
             << " finish_reason=" << response.finish_reason
             << " tool_calls="
             << (response.tool_calls.has_value() ? response.tool_calls.value().size() : 0)
             << " prompt_tokens=" << response.prompt_tokens
             << " completion_tokens=" << response.completion_tokens);
    return response;
}

StandardResponse GenieOrchestrator::executeStreamingPrepared(
    const CreateChatCompletionRequest& request,
    std::shared_ptr<ConversationSession> session,
    DraftTurn&& draft,
    IGenerativeBackend& backend,
    OrchestratorStreamCallback callback,
    const CancellationPredicate& cancel_requested,
    bool /*skip_summarization_middleware*/,
    bool register_session_hash) {
    auto& config_mgr = ModelConfigManager::getInstance();
    const bool is_vlm = config_mgr.supportsVision(request.model);
    int context_size = config_mgr.getContextSize(request.model);

    // ── Calculate reasoning budget BEFORE building prompt ─────────────────────
    bool use_reasoning = !is_vlm && config_mgr.supportsThinking(request.model);
    int thinking_budget = 0;
    int answer_budget = 0;
    int effective_max_tokens = request.max_completion_tokens.value_or(
        std::min(DEFAULT_MAX_OUTPUT_TOKENS, context_size / 2));

    if (use_reasoning) {
        // Build preliminary prompt to estimate input tokens
        std::string prelim_prompt = buildContextPrompt(*session, request);
        std::string effort = request.reasoning_effort.value_or("medium");
        ReasoningBudgetResult budget = ReasoningBudgetCalculator::compute(
            prelim_prompt,
            config_mgr.getContextSize(request.model),
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
            LOG_INFO("[GenieOrchestrator] Streaming: thinking suppressed for model "
                     << request.model << " (effort='" << effort << "')");
        } else {
            thinking_budget = budget.thinking_budget;
            answer_budget = budget.answer_budget;
            effective_max_tokens = budget.thinking_budget + budget.answer_budget;
            LOG_INFO("[GenieOrchestrator] Streaming reasoning budget for model "
                     << request.model << ": effort='" << effort
                     << "' thinking=" << thinking_budget
                     << " answer=" << answer_budget);
        }
    }

    // ── Build final prompt with budget injection ──────────────────────────────
    std::string prompt = buildContextPrompt(*session, request, thinking_budget, answer_budget);
    LOG_INFO("[GenieOrchestrator] Streaming prompt built: model="
             << request.model << " session=" << session->session_id
             << " prompt_chars=" << prompt.size());

    ensureWorkerRunning(request, backend);
    LOG_INFO("[GenieOrchestrator] Streaming backend ready: model="
             << request.model << " session=" << session->session_id
             << " max_tokens=" << effective_max_tokens
             << " reasoning=" << (use_reasoning ? "true" : "false"));

    StreamChunk role_chunk;
    role_chunk.id = session->session_id;
    role_chunk.model = request.model;
    role_chunk.role = "assistant";
    callback(role_chunk);

    const ModelConfig* model_config = config_mgr.getModelConfig(request.model);
    std::string start_tag =
        model_config ? model_config->thinking_start_tag : "<think>";
    std::string end_tag =
        model_config ? model_config->thinking_end_tag : "</think>";
    ReasoningRouter router(
        session->session_id,
        request.model,
        start_tag,
        end_tag,
        thinking_budget);

    std::string event_id = generateEventId();
    std::string full_response;
    std::string finish_reason = "stop";
    bool had_error = false;
    std::string error_msg;

    if (is_vlm) {
        ImageUtils::TempFileGuard img_guard =
            preprocessImagesToTempFiles(request.messages, request.model);
        LOG_INFO("[GenieOrchestrator] Streaming VLM execution: model="
                 << request.model
                 << " session=" << session->session_id
                 << " images=" << img_guard.paths.size());
        backend.generateVlm(
            event_id,
            prompt,
            img_guard.paths,
            true,
            effective_max_tokens,
            request.temperature.value_or(1.0f),
            request.top_p.value_or(1.0f),
            request.top_k.value_or(40),
            request.presence_penalty.value_or(0.0f),
            request.frequency_penalty.value_or(0.0f),
            [&session, &request, &callback, &full_response]
            (const IPCTokenEvent& token) {
                StreamChunk chunk;
                chunk.id = session->session_id;
                chunk.model = request.model;
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
            prompt,
            true,
            effective_max_tokens,
            request.temperature.value_or(1.0f),
            request.top_p.value_or(1.0f),
            request.top_k.value_or(40),
            request.presence_penalty.value_or(0.0f),
            request.frequency_penalty.value_or(0.0f),
            use_reasoning,
            [&use_reasoning, &router, &session, &request, &callback, &full_response]
            (const IPCTokenEvent& token) {
                if (use_reasoning) {
                    auto chunks = router.route(token.content);
                    for (auto& chunk : chunks) {
                        chunk.id = session->session_id;
                        chunk.model = request.model;
                        callback(chunk);
                        if (chunk.content_delta.has_value()) {
                            full_response += chunk.content_delta.value();
                        }
                    }
                    return;
                }

                StreamChunk chunk;
                chunk.id = session->session_id;
                chunk.model = request.model;
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
                  << request.model << " session=" << session->session_id
                  << " message=\"" << error_msg << "\"");
        throw GenAIException(GenAIErrorCode::INFERENCE_FAILED, error_msg, 500);
    }

    if (isCancellationRequested(cancel_requested)) {
        LOG_WARN("[GenieOrchestrator] Streaming inference cancelled before commit: model="
                 << request.model
                 << " session=" << session->session_id);
        return StandardResponse{};
    }

    std::string thinking_content;
    std::string answer_content = full_response;
    int reasoning_token_count = 0;
    if (use_reasoning) {
        thinking_content = router.getThinkingContent();
        answer_content = router.getAnswerContent();
        reasoning_token_count = router.getThinkingTokenCount();
    }

    json tool_calls = json::array();
    if (requestHasTools(request)) {
        const auto& adapter = ModelAdapterFactory::getAdapter(request.model);
        tool_calls = adapter.parseToolCalls(answer_content);
    }

    draft.commit(*session);
    if (use_reasoning) {
        json assistant_msg =
            {{"role", "assistant"}, {"content", answer_content}};
        if (!thinking_content.empty()) {
            assistant_msg["_thinking_content"] = thinking_content;
        }
        session->addMessage(assistant_msg);
    } else {
        session->addMessage(
            {{"role", "assistant"}, {"content", answer_content}});
    }

    // ── Post-turn memory management (LLM only, complete turns only) ───────────
    std::optional<ConversationMemoryUpdate> updated_conversation_memory;
    if (!is_vlm && tool_calls.empty()) {
        try {
            postTurnProcessing(*session, request.model, backend);
            updated_conversation_memory = makeConversationMemoryUpdate(*session);
        } catch (const std::exception& e) {
            LOG_WARN("[GenieOrchestrator] Post-turn processing failed (non-fatal): "
                     << e.what());
        }
    }

    StreamChunk finish_chunk;
    finish_chunk.id = session->session_id;
    finish_chunk.model = request.model;
    finish_chunk.finish_reason = tool_calls.empty() ? finish_reason : "tool_calls";
    callback(finish_chunk);

    if (register_session_hash) {
        registerSessionHash(request, session->session_id);
    }

    StandardResponse response;
    response.id = session->session_id;
    response.model = request.model;
    response.role = "assistant";
    response.content = answer_content;
    if (!thinking_content.empty()) {
        response.reasoning_content = thinking_content;
    }
    if (!tool_calls.empty()) {
        response.tool_calls = tool_calls;
    }
    response.finish_reason = tool_calls.empty() ? finish_reason : "tool_calls";
    response.prompt_tokens = static_cast<int>(prompt.size() / 4);
    // completion_tokens includes both answer tokens AND reasoning tokens.
    // Non-empty answers always report at least 1 token — otherwise short
    // answers (e.g. "4") truncate to 0 via integer division.
    int answer_token_estimate = answer_content.empty()
        ? 0 : static_cast<int>(answer_content.size() / 4) + 1;
    response.completion_tokens = answer_token_estimate + reasoning_token_count;
    response.reasoning_tokens = reasoning_token_count;
    // total_tokens = input + output (output already includes reasoning)
    response.total_tokens = response.prompt_tokens + response.completion_tokens;
    response.updated_conversation_memory = std::move(updated_conversation_memory);
    LOG_INFO("[GenieOrchestrator] Streaming inference completed: model="
             << request.model << " session=" << session->session_id
             << " finish_reason=" << response.finish_reason
             << " response_chars=" << answer_content.size());
    return response;
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
        std::string role = msg.value("role", "");
        if (role == "tool" || role == "system") continue;

        std::string content = msg.value("content", "");
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
        std::string role = msg.value("role", "");
        if (role != "user" && role != "assistant") continue;
        std::string content = msg.value("content", "");
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
    int current_tokens = session.estimateHistoryTokens();

    if (current_tokens <= eviction_thresh) {
        LOG_DEBUG("[GenieOrchestrator] postTurnProcessing: no eviction needed ("
                  << current_tokens << "/" << eviction_thresh << " tokens)");
        return;
    }

    // Identify eviction batch: remove oldest messages until under target.
    std::vector<json> eviction_batch;
    size_t evict_count = 0;
    int remaining_tokens = current_tokens;

    for (size_t i = session.evicted_message_count; i < session.messages.size(); ++i) {
        if (remaining_tokens <= eviction_target) break;

        const auto& msg = session.messages[i];
        std::string role = msg.value("role", "");
        if (role == "system") continue;

        std::string content = msg.value("content", "");
        int msg_tokens = static_cast<int>(content.size() / 4) + 4;

        eviction_batch.push_back(msg);
        evict_count++;
        remaining_tokens -= msg_tokens;
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
        if (!summary_text.empty()) {
            session.summary_content    = summary_text;
            session.summary_token_count = summary_tokens;
            LOG_INFO("[GenieOrchestrator] postTurnProcessing: summary updated ("
                     << summary_tokens << " tokens)");
        }
    } catch (const std::exception& e) {
        LOG_WARN("[GenieOrchestrator] postTurnProcessing: summarization failed: "
                 << e.what() << " — continuing without summary update");
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
                 << e.what() << " — continuing without facts update");
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
