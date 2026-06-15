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
#include "qai_forge/orchestration/ExclusiveLockMiddleware.h"
#include "qai_forge/orchestration/ContextCompactionMiddleware.h"
#include "qai_forge/backend/GenIEBackend.h"
#include "qai_forge/backend/BackendFactory.h"
#include "qai_forge/reasoning/ReasoningRouter.h"
#include "qai_forge/reasoning/ReasoningBudgetCalculator.h"
#include "qai_forge/adapters/ModelAdapterFactory.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/ImageUtils.h"
#include "qai_forge/utils/Logger.h"
#include <sstream>
#include <chrono>
#include <iomanip>
#include <random>
#include <mutex>

// ─────────────────────────────────────────────────────────────────────────────
// preprocessImagesToTempFiles — preprocess all image_url entries for VLM
//
// Handles both content-as-string (no images) and content-as-array format:
//   {"type": "image_url", "image_url": {"url": "..."}}
//
// Each URL is:
//   1. Downloaded (HTTP/HTTPS) or decoded (base64 data URI)
//   2. Validated (format check, size check)
//   3. Preprocessed via the full Qwen2.5-VL pipeline:
//        decode → letterbox → floor → normalize → CHW → temporal frames →
//        reshape+transpose → (L, D) float32 tensor
//   4. Written to a /tmp/vlm_pre_XXXXXX.raw temp file
//
// The VLM worker loads the .raw file and passes the float32 bytes directly to
// GenieNode_setData(GENIE_NODE_IMAGE_ENCODER_IMAGE_INPUT, ...).
//
// If only one image is provided, it is duplicated to satisfy the VLM backend's
// minimum-2-images requirement (matches Python vlm_chat_utils.py behaviour).
//
// The returned TempFileGuard deletes all temp files when it goes out of scope.
// ─────────────────────────────────────────────────────────────────────────────
static ImageUtils::TempFileGuard preprocessImagesToTempFiles(
    const json& messages,
    const std::string& model_id) {

    // Get model-specific preprocessing config from metadata.json
    VisionPreprocessConfig vision_cfg = VisionPreprocessConfig::defaults();
    auto opt_preprocess = ModelConfigManager::getInstance().getVisionPreprocessing(model_id);
    if (opt_preprocess.has_value() && !opt_preprocess->is_null()) {
        vision_cfg = VisionPreprocessConfig::fromJson(*opt_preprocess);
        LOG_DEBUG("[ChatOrchestratorImpl] Using vision_preprocessing config from metadata.json"
                  << " patch_size=" << vision_cfg.patch_size
                  << " merge_size=" << vision_cfg.merge_size
                  << " target=" << vision_cfg.target_width << "x" << vision_cfg.target_height);
    } else {
        LOG_DEBUG("[ChatOrchestratorImpl] No vision_preprocessing in metadata.json, "
                  "using Qwen2.5-VL defaults");
    }

    // Extract all image URLs from messages
    std::vector<std::string> image_urls;
    for (const auto& msg : messages) {
        if (!msg.is_object()) continue;
        const auto& content = msg.value("content", json{});
        if (!content.is_array()) continue;
        for (const auto& part : content) {
            if (!part.is_object()) continue;
            if (part.value("type", "") == "image_url") {
                auto img = part.value("image_url", json::object());
                std::string url = img.is_object() ? img.value("url", "") : img.get<std::string>();
                if (!url.empty()) {
                    image_urls.push_back(url);
                }
            }
        }
    }

    // VLM backend requires minimum 2 images — duplicate if only 1 provided
    // (matches Python vlm_chat_utils.py behaviour)
    if (image_urls.size() == 1) {
        LOG_INFO("[ChatOrchestratorImpl] Only 1 image provided, duplicating for VLM "
                 "backend minimum-2-images requirement");
        image_urls.push_back(image_urls[0]);
    }

    // Preprocess each image and write to temp file
    ImageUtils::TempFileGuard guard;
    for (size_t i = 0; i < image_urls.size(); ++i) {
        const auto& url = image_urls[i];
        try {
            std::string temp_path = ImageUtils::preprocessImageToTempFile(url, vision_cfg, model_id);
            guard.paths.push_back(temp_path);
            LOG_INFO("[ChatOrchestratorImpl] Preprocessed image " << (i + 1)
                     << "/" << image_urls.size() << " → " << temp_path);
        } catch (const GenAIException&) {
            throw;  // Already formatted correctly
        } catch (const std::exception& e) {
            throw GenAIException(GenAIErrorCode::INVALID_REQUEST,
                std::string("Failed to preprocess image ") + std::to_string(i + 1) +
                ": " + e.what(), 400);
        }
    }

    return guard;
}

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
// Constructor — initialize backend_ and build middleware pipeline
// ─────────────────────────────────────────────────────────────────────────────
ChatOrchestratorImpl::ChatOrchestratorImpl()
    : backend_(GenIEBackend::getInstance())
{
    buildMiddlewarePipeline();
}

// ─────────────────────────────────────────────────────────────────────────────
// buildMiddlewarePipeline — construct pipeline from BackendCapabilities
//
// This is the key Phase 2 change: the middleware pipeline is built dynamically
// from backend capabilities instead of being hardcoded in handleBlocking() and
// handleStreaming(). Adding a new backend with different capabilities requires
// zero changes to this method — only BackendCapabilities changes.
// ─────────────────────────────────────────────────────────────────────────────
void ChatOrchestratorImpl::buildMiddlewarePipeline() {
    const BackendCapabilities caps = backend_.capabilities();

    middleware_pipeline_.clear();

    // Step 1: Concurrency middleware — shape depends on backend
    switch (caps.concurrency_model) {
        case ConcurrencyModel::EXCLUSIVE:
            // GenIE, LiteRT LM: one inference at a time (DSP/GPU hardware lock)
            middleware_pipeline_.push_back(
                std::make_unique<ExclusiveLockMiddleware>(300000));
            LOG_INFO("[ChatOrchestratorImpl] Middleware: ExclusiveLockMiddleware (timeout=300s)");
            break;
        case ConcurrencyModel::BOUNDED:
            // OnnxRT (future): up to N concurrent inferences
            // BoundedConcurrencyMiddleware will be added in Phase 6
            LOG_INFO("[ChatOrchestratorImpl] Middleware: BoundedConcurrency (max="
                     << caps.max_concurrent << ") — not yet implemented, no lock");
            break;
        case ConcurrencyModel::UNLIMITED:
            // CPU-only: no concurrency limit
            LOG_INFO("[ChatOrchestratorImpl] Middleware: no concurrency limit");
            break;
    }

    // Step 2: Context compaction middleware — always added
    // ContextCompactionMiddleware calls backend.onContextCompacted() after
    // summarization — GenIE: sendReset(); OnnxRT: no-op.
    middleware_pipeline_.push_back(
        std::make_unique<ContextCompactionMiddleware>(
            caps.context_window,
            caps.compaction_threshold,
            caps.context_strategy));
    LOG_INFO("[ChatOrchestratorImpl] Middleware: ContextCompactionMiddleware"
             << " (window=" << caps.context_window
             << " threshold=" << caps.compaction_threshold
             << " strategy=" << (caps.context_strategy == ContextStrategy::RESET_KV
                                 ? "RESET_KV" : "FULL_RECOMPUTE") << ")");

    LOG_INFO("[ChatOrchestratorImpl] Middleware pipeline built: "
             << middleware_pipeline_.size() << " stage(s) for backend '"
             << backend_.name() << "'");
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

// ─────────────────────────────────────────────────────────────────────────────
// generateEventId — unique ID for each inference event
// ─────────────────────────────────────────────────────────────────────────────
static std::string generateEventId() {
    static std::mt19937_64 rng(std::random_device{}());
    std::ostringstream oss;
    oss << "evt-" << std::hex << rng();
    return oss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// handleBlocking — P1: Wired to InferenceWorkerManager
// ─────────────────────────────────────────────────────────────────────────────
StandardResponse ChatOrchestratorImpl::handleBlocking(const CreateChatCompletionRequest& request) {
    // Step 1: Validate
    validateRequest(request);

    // Step 2: Resolve session and draft
    auto [session, draft] = resolveSessionAndDraft(request);

    // Step 3: Middleware — P3: Acquire DSP lock before any inference
    ConcurrencyMiddleware::Guard dsp_lock(session->session_id, 300000);

    // P4: SummarizationMiddleware — check and summarize if needed.
    // backend_.onContextCompacted() is called inside if summarization occurs.
    auto& config_mgr_s = ModelConfigManager::getInstance();
    int context_size_s = config_mgr_s.getContextSize(request.model);
    SummarizationMiddleware::checkAndSummarize(*session, request, backend_, context_size_s);

    // Step 4: Build context prompt
    std::string prompt = buildContextPrompt(*session, request);

    // Determine model capabilities
    auto& config_mgr = ModelConfigManager::getInstance();
    bool use_reasoning = config_mgr.supportsThinking(request.model);
    bool is_vlm = config_mgr.supportsVision(request.model);

    // ── Context-aware reasoning budget ────────────────────────────────────────
    // Compute thinking budget AFTER prompt assembly so all overhead is accounted
    // for (system prompt, tool definitions, history, user message).
    int effective_max_tokens = request.max_completion_tokens.value_or(1024);
    if (use_reasoning && !is_vlm) {
        std::string effort = request.reasoning_effort.value_or("medium");
        ReasoningBudgetResult budget = ReasoningBudgetCalculator::compute(
            prompt,
            config_mgr.getContextSize(request.model),
            effort,
            request.max_completion_tokens
        );
        if (budget.context_too_small) {
            throw GenAIException(GenAIErrorCode::CONTEXT_LENGTH_EXCEEDED,
                "Context window is too small for this request. "
                "Reduce the conversation history or use a model with a larger context window.",
                400);
        }
        if (budget.suppress_thinking) {
            use_reasoning = false;
            LOG_INFO("[ChatOrchestratorImpl] Thinking suppressed for model " << request.model
                     << " (effort='" << effort << "', budget=" << budget.thinking_budget << ")");
        } else {
            LOG_INFO("[ChatOrchestratorImpl] Reasoning budget for model " << request.model
                     << ": effort='" << effort << "'"
                     << " thinking=" << budget.thinking_budget
                     << " answer=" << budget.answer_budget
                     << " input_est=" << budget.estimated_input_tokens);
        }
        effective_max_tokens = budget.answer_budget;
    }

    // Ensure the correct worker subprocess is running
    const ModelConfig* mc_blocking = config_mgr.getModelConfig(request.model);
    std::string config_file = mc_blocking ? mc_blocking->config_file : "";
    std::string sampler_file = mc_blocking ? mc_blocking->sampler_config_file : "";
    backend_.ensureWorkerRunning(request.model, config_file, sampler_file);

    // Execute inference (blocking — accumulate full response)
    std::string event_id = generateEventId();
    std::string full_response;
    std::string finish_reason = "stop";
    bool had_error = false;
    std::string error_msg;

    if (is_vlm) {
        // ── VLM model: preprocess images, delegate to backend_.generateVlm() ──
        ImageUtils::TempFileGuard img_guard = preprocessImagesToTempFiles(
            request.messages, request.model);
        LOG_INFO("[ChatOrchestratorImpl] VLM blocking request for model " << request.model
                 << " with " << img_guard.paths.size() << " preprocessed image(s)");
        backend_.generateVlm(
            event_id, prompt, img_guard.paths, false,
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
            }
        );
    } else {
        // ── LLM model: delegate to backend_.generate() ────────────────────────
        // use_reasoning is passed to the backend; GenIEBackend maps it to
        // bypass_think_filter internally — invisible to this orchestrator.
        backend_.generate(
            event_id, prompt, false,
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
            }
        );
    }

    if (had_error) {
        throw GenAIException(GenAIErrorCode::INFERENCE_FAILED, error_msg, 500);
    }

    // For non-streaming reasoning models, extract thinking from response.
    // ReasoningRouter is backend-agnostic — it processes raw token text regardless
    // of which backend produced it.
    std::string thinking_content;
    std::string answer_content = full_response;
    int reasoning_token_count = 0;
    if (use_reasoning) {
        // Use ReasoningRouter to split thinking from answer.
        // Use model-config tags instead of hardcoded "<think>"/"</think>"
        const ModelConfig* mc_tags = config_mgr.getModelConfig(request.model);
        std::string start_tag = mc_tags ? mc_tags->thinking_start_tag : "<think>";
        std::string end_tag   = mc_tags ? mc_tags->thinking_end_tag   : "</think>";
        ReasoningRouter router(session->session_id, request.model, start_tag, end_tag);
        router.route(full_response);
        thinking_content      = router.getThinkingContent();
        answer_content        = router.getAnswerContent();
        reasoning_token_count = router.getThinkingTokenCount();
    }

    // Check for tool calls via model adapter
    const auto& adapter = ModelAdapterFactory::getAdapter(request.model);
    json tool_calls = adapter.parseToolCalls(answer_content);

    // Commit draft to session history
    draft.commit(*session);
    json assistant_msg = {{"role", "assistant"}, {"content", answer_content}};
    if (!thinking_content.empty()) {
        assistant_msg["_thinking_content"] = thinking_content;
    }
    session->addMessage(assistant_msg);

    // Register session hash
    auto& session_mgr = SessionManager::getInstance();
    std::string hash = SessionManager::calculateMessagesHash(request.messages);
    session_mgr.registerHash(hash, session->session_id);

    // Build response DTO
    StandardResponse response;
    response.id = session->session_id;
    response.model = request.model;
    response.role = "assistant";
    response.content = answer_content;
    if (!thinking_content.empty()) response.reasoning_content = thinking_content;
    if (!tool_calls.empty()) response.tool_calls = tool_calls;
    response.finish_reason = tool_calls.empty() ? finish_reason : "tool_calls";
    response.prompt_tokens    = static_cast<int>(prompt.size() / 4);
    // completion_tokens counts only the answer tokens (not thinking tokens).
    // reasoning_tokens is reported separately in output_tokens_details.
    response.completion_tokens = static_cast<int>(answer_content.size() / 4);
    response.reasoning_tokens  = reasoning_token_count;
    response.total_tokens      = response.prompt_tokens
                                + response.completion_tokens
                                + response.reasoning_tokens;

    return response;
}

// ─────────────────────────────────────────────────────────────────────────────
// handleStreaming — P1 + P2: Wired to InferenceWorkerManager + ReasoningRouter
// ─────────────────────────────────────────────────────────────────────────────
void ChatOrchestratorImpl::handleStreaming(const CreateChatCompletionRequest& request,
                                           StreamCallback callback) {
    // Step 1: Validate
    validateRequest(request);

    // Step 2: Resolve session and draft
    auto [session, draft] = resolveSessionAndDraft(request);

    // Step 3: Middleware — P3: Acquire DSP lock before any inference
    ConcurrencyMiddleware::Guard dsp_lock(session->session_id, 300000);

    // P4: SummarizationMiddleware — check and summarize if needed.
    // backend_.onContextCompacted() is called inside if summarization occurs.
    auto& config_mgr_s2 = ModelConfigManager::getInstance();
    int context_size_s2 = config_mgr_s2.getContextSize(request.model);
    SummarizationMiddleware::checkAndSummarize(*session, request, backend_, context_size_s2);

    // Step 4: Build context prompt
    std::string prompt = buildContextPrompt(*session, request);

    // Determine model capabilities
    auto& config_mgr = ModelConfigManager::getInstance();
    bool use_reasoning = config_mgr.supportsThinking(request.model);
    bool is_vlm = config_mgr.supportsVision(request.model);

    // ── Context-aware reasoning budget (streaming) ────────────────────────────
    int effective_max_tokens_s = request.max_completion_tokens.value_or(1024);
    int thinking_budget_s = -1;  // -1 = unlimited (default for ReasoningRouter)
    if (use_reasoning && !is_vlm) {
        std::string effort_s = request.reasoning_effort.value_or("medium");
        ReasoningBudgetResult budget_s = ReasoningBudgetCalculator::compute(
            prompt,
            config_mgr.getContextSize(request.model),
            effort_s,
            request.max_completion_tokens
        );
        if (budget_s.context_too_small) {
            throw GenAIException(GenAIErrorCode::CONTEXT_LENGTH_EXCEEDED,
                "Context window is too small for this request. "
                "Reduce the conversation history or use a model with a larger context window.",
                400);
        }
        if (budget_s.suppress_thinking) {
            use_reasoning = false;
            LOG_INFO("[ChatOrchestratorImpl] Streaming: thinking suppressed for model "
                     << request.model << " (effort='" << effort_s << "')");
        } else {
            thinking_budget_s = budget_s.thinking_budget;
            effective_max_tokens_s = budget_s.answer_budget;
            LOG_INFO("[ChatOrchestratorImpl] Streaming reasoning budget for model "
                     << request.model
                     << ": effort='" << effort_s << "'"
                     << " thinking=" << thinking_budget_s
                     << " answer=" << effective_max_tokens_s);
        }
    }

    // Ensure the correct worker subprocess is running
    const ModelConfig* mc_streaming = config_mgr.getModelConfig(request.model);
    std::string config_file = mc_streaming ? mc_streaming->config_file : "";
    std::string sampler_file_s = mc_streaming ? mc_streaming->sampler_config_file : "";
    backend_.ensureWorkerRunning(request.model, config_file, sampler_file_s);

    // Emit role chunk first
    StreamChunk role_chunk;
    role_chunk.id = session->session_id;
    role_chunk.model = request.model;
    role_chunk.role = "assistant";
    callback(role_chunk);

    // ReasoningRouter is created for models that support thinking.
    // It is backend-agnostic — processes raw token text from any backend.
    std::string event_id = generateEventId();
    std::string full_response;
    std::string finish_reason = "stop";
    bool had_error = false;
    std::string error_msg;

    if (is_vlm) {
        // ── VLM model: preprocess images, delegate to backend_.generateVlm() ──
        ImageUtils::TempFileGuard img_guard = preprocessImagesToTempFiles(
            request.messages, request.model);
        LOG_INFO("[ChatOrchestratorImpl] VLM streaming request for model " << request.model
                 << " with " << img_guard.paths.size() << " preprocessed image(s)");
        backend_.generateVlm(
            event_id, prompt, img_guard.paths, true,
            effective_max_tokens_s,
            request.temperature.value_or(1.0f),
            request.top_p.value_or(1.0f),
            request.top_k.value_or(40),
            request.presence_penalty.value_or(0.0f),
            request.frequency_penalty.value_or(0.0f),
            [&session, &callback, &full_response](const IPCTokenEvent& token) {
                StreamChunk chunk;
                chunk.id = session->session_id;
                chunk.model = session->current_model_id;
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
            }
        );

        if (!had_error) {
            draft.commit(*session);
            session->addMessage({{"role", "assistant"}, {"content", full_response}});
        }

    } else {
        // ── LLM model (standard or reasoning): delegate to backend_.generate() ─
        // use_reasoning is passed to the backend; GenIEBackend maps it to
        // bypass_think_filter internally — invisible to this orchestrator.
        // ReasoningRouter handles <think> token routing for all backends.
        const ModelConfig* mc_tags_s = config_mgr.getModelConfig(request.model);
        std::string start_tag_s = mc_tags_s ? mc_tags_s->thinking_start_tag : "<think>";
        std::string end_tag_s   = mc_tags_s ? mc_tags_s->thinking_end_tag   : "</think>";
        ReasoningRouter router(session->session_id, request.model, start_tag_s, end_tag_s);

        backend_.generate(
            event_id, prompt, true,
            effective_max_tokens_s,
            request.temperature.value_or(1.0f),
            request.top_p.value_or(1.0f),
            request.top_k.value_or(40),
            request.presence_penalty.value_or(0.0f),
            request.frequency_penalty.value_or(0.0f),
            use_reasoning,
            [&use_reasoning, &router, &session, &callback, &full_response]
            (const IPCTokenEvent& token) {
                if (use_reasoning) {
                    // Route through ReasoningRouter — splits <think> from answer
                    auto chunks = router.route(token.content);
                    for (const auto& chunk : chunks) {
                        callback(chunk);
                        if (chunk.content_delta.has_value())
                            full_response += chunk.content_delta.value();
                    }
                } else {
                    // Standard LLM — tokens go directly to content_delta
                    StreamChunk chunk;
                    chunk.id = session->session_id;
                    chunk.model = session->current_model_id;
                    chunk.content_delta = token.content;
                    full_response += token.content;
                    callback(chunk);
                }
            },
            [&finish_reason](const IPCDoneEvent& done) {
                finish_reason = done.finish_reason;
            },
            [&had_error, &error_msg](const IPCErrorEvent& err) {
                had_error = true;
                error_msg = err.message;
            }
        );

        if (!had_error) {
            draft.commit(*session);
            if (use_reasoning) {
                json assistant_msg = {{"role", "assistant"}, {"content", router.getAnswerContent()}};
                if (!router.getThinkingContent().empty()) {
                    assistant_msg["_thinking_content"] = router.getThinkingContent();
                }
                session->addMessage(assistant_msg);
            } else {
                session->addMessage({{"role", "assistant"}, {"content", full_response}});
            }
        }
    }

    if (had_error) {
        throw GenAIException(GenAIErrorCode::INFERENCE_FAILED, error_msg, 500);
    }

    // Emit finish chunk
    StreamChunk finish_chunk;
    finish_chunk.id = session->session_id;
    finish_chunk.model = request.model;
    finish_chunk.finish_reason = finish_reason;
    callback(finish_chunk);

    // Register session hash
    auto& session_mgr = SessionManager::getInstance();
    std::string hash = SessionManager::calculateMessagesHash(request.messages);
    session_mgr.registerHash(hash, session->session_id);
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
