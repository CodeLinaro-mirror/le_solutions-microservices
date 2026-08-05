// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// ChatOrchestratorImpl — Layer 2 Pipeline Implementation
//
// P1: InferenceWorkerManager is now wired into handleBlocking() and
//     handleStreaming(). The stub responses have been replaced with actual
//     calls to InferenceWorkerManager::executeRequest().
//
// P2: ReasoningRouter is now wired into handleStreaming(). When the model
//     supports thinking (bypass_think_filter=true), raw tokens are routed
//     through ReasoningRouter which yields StreamChunk(reasoning_content=...)
//     or StreamChunk(content_delta=...) DTOs.
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/orchestration/ChatOrchestratorImpl.h"
#include "qai_forge/orchestration/ConcurrencyMiddleware.h"
#include "qai_forge/orchestration/SummarizationMiddleware.h"
#include "qai_forge/backend/GenIEBackend.h"
#include "qai_forge/reasoning/ReasoningRouter.h"
#include "qai_forge/reasoning/ReasoningBudgetCalculator.h"
#include "qai_forge/adapters/ModelAdapterFactory.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/ImageUtils.h"
#include "qai_forge/utils/Logger.h"
#include <exception>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <random>
#include <mutex>
#include <utility>
#include <vector>

namespace {

bool isCancellationRequested(
    const ChatOrchestratorImpl::CancellationPredicate& cancel_requested) {
    return cancel_requested && cancel_requested();
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

ImageUtils::TempFileGuard preprocessImagesToTempFiles(
    const json& messages,
    const std::string& model_id) {
    VisionPreprocessConfig vision_cfg = VisionPreprocessConfig::defaults();
    auto opt_preprocess =
        ModelConfigManager::getInstance().getVisionPreprocessing(model_id);
    if (opt_preprocess.has_value() && !opt_preprocess->is_null()) {
        vision_cfg = VisionPreprocessConfig::fromJson(*opt_preprocess);
        LOG_DEBUG("[ChatOrchestratorImpl] Using vision_preprocessing config: model="
                  << model_id
                  << " patch_size=" << vision_cfg.patch_size
                  << " merge_size=" << vision_cfg.merge_size
                  << " target=" << vision_cfg.target_width << "x"
                  << vision_cfg.target_height);
    } else {
        LOG_DEBUG("[ChatOrchestratorImpl] No vision_preprocessing in metadata.json; "
                  "using default VLM preprocessing config for model="
                  << model_id);
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
            if (part.value("type", "") != "image_url") {
                continue;
            }

            auto img = part.value("image_url", json::object());
            if (img.is_object()) {
                std::string url = img.value("url", "");
                if (!url.empty()) {
                    image_urls.push_back(url);
                }
                continue;
            }
            std::string url = img.get<std::string>();
            if (!url.empty()) {
                image_urls.push_back(url);
            }
        }
    }

    if (image_urls.size() == 1) {
        LOG_INFO("[ChatOrchestratorImpl] Only one image supplied for VLM; "
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
            LOG_INFO("[ChatOrchestratorImpl] Preprocessed VLM image "
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
// ChatOrchestrator::getInstance() — factory method wires to the concrete impl
// ─────────────────────────────────────────────────────────────────────────────
ChatOrchestrator& ChatOrchestrator::getInstance() {
    return ChatOrchestratorImpl::getInstance();
}

ChatOrchestratorImpl& ChatOrchestratorImpl::getInstance() {
    static ChatOrchestratorImpl instance;
    return instance;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor — initialize the legacy fallback backend reference
// ─────────────────────────────────────────────────────────────────────────────
ChatOrchestratorImpl::ChatOrchestratorImpl()
    : backend_(GenIEBackend::getInstance())
{
}

// ─────────────────────────────────────────────────────────────────────────────
// Step 1: validateRequest
// ─────────────────────────────────────────────────────────────────────────────
void ChatOrchestratorImpl::validateRequest(const CreateChatCompletionRequest& request) {
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
ChatOrchestratorImpl::resolveSessionAndDraft(const CreateChatCompletionRequest& request) {
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
// Step 3a: isToolContinuation (ToolMiddleware)
// ─────────────────────────────────────────────────────────────────────────────
bool ChatOrchestratorImpl::isToolContinuation(const ConversationSession& session,
                                               const CreateChatCompletionRequest& request) const {
    if (request.messages.empty()) return false;
    const auto& last_msg = request.messages.back();
    if (!last_msg.is_object()) return false;
    return last_msg.value("role", "") == "tool";
}

// ─────────────────────────────────────────────────────────────────────────────
// buildContextPrompt — Context Compaction (Section 6)
// ─────────────────────────────────────────────────────────────────────────────
std::string ChatOrchestratorImpl::buildContextPrompt(const ConversationSession& session,
                                                      const CreateChatCompletionRequest& request) const {
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

    // 1. System prompt (with tool instructions injected by adapter)
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

    // 2. Summary (if available)
    if (!session.summary_content.empty()) {
        prompt << system_prefix << "[Summary of previous conversation]: "
               << session.summary_content << system_suffix;
    }

    // 3. Clean message history (strips private "_*" keys like "_thinking_content")
    auto clean_messages = session.getCleanMessages();
    for (const auto& msg : clean_messages) {
        std::string role = msg.value("role", "");
        std::string content = msg.value("content", "");
        if (role == "user") {
            prompt << user_prefix << content << user_suffix;
        } else if (role == "assistant") {
            prompt << assistant_prefix << content << assistant_suffix;
        }
    }

    // 4. New messages from the draft (preprocessed by model adapter for vision)
    json processed_messages = adapter.preprocessVision(request.messages);
    for (const auto& msg : processed_messages) {
        std::string role = msg.value("role", "");
        std::string content = msg.value("content", "");
        if (role == "user") {
            prompt << user_prefix << content << user_suffix;
        } else if (role == "tool") {
            prompt << user_prefix << adapter.formatToolResponse(json::array({msg})) << user_suffix;
        }
    }

    // 5. Assistant turn start
    prompt << assistant_prefix;

    return prompt.str();
}

StandardResponse ChatOrchestratorImpl::executeBlocking(
    const CreateChatCompletionRequest& request,
    IGenerativeBackend& backend,
    CancellationPredicate cancel_requested,
    bool skip_summarization_middleware) {
    validateRequest(request);
    auto [session, draft] = resolveSessionAndDraft(request);
    LOG_INFO("[ChatOrchestratorImpl] Prepared blocking request: model="
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

StandardResponse ChatOrchestratorImpl::executeStreaming(
    const CreateChatCompletionRequest& request,
    IGenerativeBackend& backend,
    StreamCallback callback,
    CancellationPredicate cancel_requested,
    bool skip_summarization_middleware) {
    validateRequest(request);
    auto [session, draft] = resolveSessionAndDraft(request);
    LOG_INFO("[ChatOrchestratorImpl] Prepared streaming request: model="
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

StandardResponse ChatOrchestratorImpl::executeFromMessages(
    const CreateChatCompletionRequest& request,
    const json& response_history,
    IGenerativeBackend& backend,
    CancellationPredicate cancel_requested,
    bool skip_summarization_middleware) {
    validateRequest(request);
    const std::string session_id = request.user.value_or("");
    auto session = std::make_shared<ConversationSession>(
        session_id.empty() ? request.model : session_id);
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

    return executeBlockingPrepared(
        request,
        std::move(session),
        std::move(draft),
        backend,
        cancel_requested,
        skip_summarization_middleware,
        false);
}

StandardResponse ChatOrchestratorImpl::executeFromMessages(
    const CreateChatCompletionRequest& request,
    const json& response_history,
    IGenerativeBackend& backend,
    StreamCallback callback,
    CancellationPredicate cancel_requested,
    bool skip_summarization_middleware) {
    validateRequest(request);
    const std::string session_id = request.user.value_or("");
    auto session = std::make_shared<ConversationSession>(
        session_id.empty() ? request.model : session_id);
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

    return executeStreamingPrepared(
        request,
        std::move(session),
        std::move(draft),
        backend,
        std::move(callback),
        cancel_requested,
        skip_summarization_middleware,
        false);
}

StandardResponse ChatOrchestratorImpl::executeBlockingPrepared(
    const CreateChatCompletionRequest& request,
    std::shared_ptr<ConversationSession> session,
    DraftTurn&& draft,
    IGenerativeBackend& backend,
    const CancellationPredicate& cancel_requested,
    bool skip_summarization_middleware,
    bool register_session_hash) {
    auto& config_mgr = ModelConfigManager::getInstance();
    const bool is_vlm = config_mgr.supportsVision(request.model);
    int context_size = config_mgr.getContextSize(request.model);
    if (!skip_summarization_middleware) {
        SummarizationMiddleware::checkAndSummarize(
            *session,
            request,
            backend,
            context_size);
    }

    std::string prompt = buildContextPrompt(*session, request);
    LOG_INFO("[ChatOrchestratorImpl] Blocking prompt built: model="
             << request.model << " session=" << session->session_id
             << " prompt_chars=" << prompt.size());

    bool use_reasoning = !is_vlm && config_mgr.supportsThinking(request.model);
    int effective_max_tokens = request.max_completion_tokens.value_or(1024);
    if (use_reasoning) {
        std::string effort = request.reasoning_effort.value_or("medium");
        ReasoningBudgetResult budget = ReasoningBudgetCalculator::compute(
            prompt,
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
            LOG_INFO("[ChatOrchestratorImpl] Thinking suppressed for model "
                     << request.model << " (effort='" << effort
                     << "', budget=" << budget.thinking_budget << ")");
        } else {
            LOG_INFO("[ChatOrchestratorImpl] Reasoning budget for model "
                     << request.model << ": effort='" << effort
                     << "' thinking=" << budget.thinking_budget
                     << " answer=" << budget.answer_budget
                     << " input_est=" << budget.estimated_input_tokens);
        }
        effective_max_tokens = budget.answer_budget;
    }

    ensureWorkerRunning(request, backend);
    LOG_INFO("[ChatOrchestratorImpl] Blocking backend ready: model="
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
        LOG_INFO("[ChatOrchestratorImpl] Blocking VLM execution: model="
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

    if (had_error) {
        LOG_ERROR("[ChatOrchestratorImpl] Blocking inference failed: model="
                  << request.model << " session=" << session->session_id
                  << " message=\"" << error_msg << "\"");
        throw GenAIException(GenAIErrorCode::INFERENCE_FAILED, error_msg, 500);
    }

    if (isCancellationRequested(cancel_requested)) {
        LOG_WARN("[ChatOrchestratorImpl] Blocking inference cancelled before commit: model="
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
            end_tag);
        router.route(full_response);
        thinking_content = router.getThinkingContent();
        answer_content = router.getAnswerContent();
        reasoning_token_count = router.getThinkingTokenCount();
    }

    const auto& adapter = ModelAdapterFactory::getAdapter(request.model);
    json tool_calls = adapter.parseToolCalls(answer_content);

    if (isCancellationRequested(cancel_requested)) {
        LOG_WARN("[ChatOrchestratorImpl] Blocking inference cancelled before session update: model="
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
    response.completion_tokens = static_cast<int>(answer_content.size() / 4);
    response.reasoning_tokens = reasoning_token_count;
    response.total_tokens = response.prompt_tokens +
                            response.completion_tokens +
                            response.reasoning_tokens;
    LOG_INFO("[ChatOrchestratorImpl] Blocking inference completed: model="
             << request.model << " session=" << session->session_id
             << " finish_reason=" << response.finish_reason
             << " tool_calls="
             << (response.tool_calls.has_value() ? response.tool_calls.value().size() : 0)
             << " prompt_tokens=" << response.prompt_tokens
             << " completion_tokens=" << response.completion_tokens);
    return response;
}

StandardResponse ChatOrchestratorImpl::executeStreamingPrepared(
    const CreateChatCompletionRequest& request,
    std::shared_ptr<ConversationSession> session,
    DraftTurn&& draft,
    IGenerativeBackend& backend,
    StreamCallback callback,
    const CancellationPredicate& cancel_requested,
    bool skip_summarization_middleware,
    bool register_session_hash) {
    auto& config_mgr = ModelConfigManager::getInstance();
    const bool is_vlm = config_mgr.supportsVision(request.model);
    int context_size = config_mgr.getContextSize(request.model);
    if (!skip_summarization_middleware) {
        SummarizationMiddleware::checkAndSummarize(
            *session,
            request,
            backend,
            context_size);
    }

    std::string prompt = buildContextPrompt(*session, request);
    LOG_INFO("[ChatOrchestratorImpl] Streaming prompt built: model="
             << request.model << " session=" << session->session_id
             << " prompt_chars=" << prompt.size());

    bool use_reasoning = !is_vlm && config_mgr.supportsThinking(request.model);
    int effective_max_tokens = request.max_completion_tokens.value_or(1024);
    if (use_reasoning) {
        std::string effort = request.reasoning_effort.value_or("medium");
        ReasoningBudgetResult budget = ReasoningBudgetCalculator::compute(
            prompt,
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
            LOG_INFO("[ChatOrchestratorImpl] Streaming: thinking suppressed for model "
                     << request.model << " (effort='" << effort << "')");
        } else {
            effective_max_tokens = budget.answer_budget;
            LOG_INFO("[ChatOrchestratorImpl] Streaming reasoning budget for model "
                     << request.model << ": effort='" << effort
                     << "' thinking=" << budget.thinking_budget
                     << " answer=" << effective_max_tokens);
        }
    }

    ensureWorkerRunning(request, backend);
    LOG_INFO("[ChatOrchestratorImpl] Streaming backend ready: model="
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
        end_tag);

    std::string event_id = generateEventId();
    std::string full_response;
    std::string finish_reason = "stop";
    bool had_error = false;
    std::string error_msg;

    if (is_vlm) {
        ImageUtils::TempFileGuard img_guard =
            preprocessImagesToTempFiles(request.messages, request.model);
        LOG_INFO("[ChatOrchestratorImpl] Streaming VLM execution: model="
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

    if (had_error) {
        LOG_ERROR("[ChatOrchestratorImpl] Streaming inference failed: model="
                  << request.model << " session=" << session->session_id
                  << " message=\"" << error_msg << "\"");
        throw GenAIException(GenAIErrorCode::INFERENCE_FAILED, error_msg, 500);
    }

    if (isCancellationRequested(cancel_requested)) {
        LOG_WARN("[ChatOrchestratorImpl] Streaming inference cancelled before commit: model="
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

    const auto& adapter = ModelAdapterFactory::getAdapter(request.model);
    json tool_calls = adapter.parseToolCalls(answer_content);

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
    response.completion_tokens = static_cast<int>(answer_content.size() / 4);
    response.reasoning_tokens = reasoning_token_count;
    response.total_tokens = response.prompt_tokens +
                            response.completion_tokens +
                            response.reasoning_tokens;
    LOG_INFO("[ChatOrchestratorImpl] Streaming inference completed: model="
             << request.model << " session=" << session->session_id
             << " finish_reason=" << response.finish_reason
             << " response_chars=" << answer_content.size());
    return response;
}

// ─────────────────────────────────────────────────────────────────────────────
// handleBlocking — P1: Wired to InferenceWorkerManager
// ─────────────────────────────────────────────────────────────────────────────
StandardResponse ChatOrchestratorImpl::handleBlocking(const CreateChatCompletionRequest& request) {
    validateRequest(request);
    auto [session, draft] = resolveSessionAndDraft(request);
    LOG_INFO("[ChatOrchestratorImpl] Legacy blocking path acquiring global guard: model="
             << request.model << " session=" << session->session_id
             << " vision="
             << (ModelConfigManager::getInstance().supportsVision(request.model)
                     ? "true"
                     : "false"));
    ConcurrencyMiddleware::Guard dsp_lock(session->session_id, 300000);
    return executeBlockingPrepared(
        request,
        std::move(session),
        std::move(draft),
        backend_,
        {},
        false,
        true);
}

// ─────────────────────────────────────────────────────────────────────────────
// handleStreaming — P1 + P2: Wired to InferenceWorkerManager + ReasoningRouter
// ─────────────────────────────────────────────────────────────────────────────
void ChatOrchestratorImpl::handleStreaming(const CreateChatCompletionRequest& request,
                                           StreamCallback callback) {
    validateRequest(request);
    auto [session, draft] = resolveSessionAndDraft(request);
    LOG_INFO("[ChatOrchestratorImpl] Legacy streaming path acquiring global guard: model="
             << request.model << " session=" << session->session_id
             << " vision="
             << (ModelConfigManager::getInstance().supportsVision(request.model)
                     ? "true"
                     : "false"));
    ConcurrencyMiddleware::Guard dsp_lock(session->session_id, 300000);
    executeStreamingPrepared(
        request,
        std::move(session),
        std::move(draft),
        backend_,
        std::move(callback),
        {},
        false,
        true);
}

// ─────────────────────────────────────────────────────────────────────────────
// deleteSession
// ─────────────────────────────────────────────────────────────────────────────
bool ChatOrchestratorImpl::deleteSession(const std::string& completion_id) {
    return SessionManager::getInstance().deleteSession(completion_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// cancelSession
// ─────────────────────────────────────────────────────────────────────────────
bool ChatOrchestratorImpl::cancelSession(const std::string& completion_id) {
    auto session = SessionManager::getInstance().getSession(completion_id);
    if (!session) return false;

    LOG_INFO("[ChatOrchestratorImpl] Cancelling session: " << completion_id);
    // Delegate to the backend — GenIEBackend selects the correct worker
    // (LLM or VLM) based on current_is_vlm_ and sends SIGKILL.
    backend_.terminateWorker(/*force=*/true);
    return true;
}
