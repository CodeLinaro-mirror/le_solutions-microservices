// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/orchestration/LiteRTLMOrchestrator.h"

#include "qai_forge/backend/LiteRTLMBackend.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/reasoning/ReasoningRouter.h"
#include "qai_forge/utils/Logger.h"

#include <jinja2cpp/template.h>
#include <jinja2cpp/value.h>

#include <cctype>
#include <iomanip>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {

std::string generateEventId() {
    static std::mt19937_64 rng(std::random_device{}());
    std::ostringstream oss;
    oss << "litert-lm-" << std::hex << std::setw(16) << std::setfill('0')
        << rng();
    return oss.str();
}

// Recursively converts an nlohmann::json value into a jinja2::Value so it
// can be passed as template render context to jinja2cpp. Handles the shapes
// produced by CreateChatCompletionRequest: arrays of message objects,
// nested tool-call objects, primitive scalars, etc.
jinja2::Value jsonToJinjaValue(const json& value) {
    if (value.is_null()) {
        return jinja2::Value();
    }
    if (value.is_boolean()) {
        return jinja2::Value(value.get<bool>());
    }
    if (value.is_number_integer()) {
        return jinja2::Value(static_cast<int64_t>(value.get<long long>()));
    }
    if (value.is_number_unsigned()) {
        return jinja2::Value(static_cast<int64_t>(value.get<unsigned long long>()));
    }
    if (value.is_number_float()) {
        return jinja2::Value(value.get<double>());
    }
    if (value.is_string()) {
        return jinja2::Value(value.get<std::string>());
    }
    if (value.is_array()) {
        jinja2::ValuesList list;
        list.reserve(value.size());
        for (const auto& item : value) {
            list.push_back(jsonToJinjaValue(item));
        }
        return jinja2::Value(std::move(list));
    }
    if (value.is_object()) {
        jinja2::ValuesMap map;
        for (auto it = value.begin(); it != value.end(); ++it) {
            map[it.key()] = jsonToJinjaValue(it.value());
        }
        return jinja2::Value(std::move(map));
    }
    return jinja2::Value();
}

} // namespace

float LiteRTLMOrchestrator::compactionThreshold() {
    const char* env = std::getenv("LITERT_LM_COMPACTION_THRESHOLD");
    if (env) {
        float val = std::stof(env);
        if (val > 0.0f && val < 1.0f) return val;
    }
    return 0.75f;
}

int LiteRTLMOrchestrator::defaultContextLength() {
    const char* env = std::getenv("LITERT_LM_DEFAULT_CONTEXT_LENGTH");
    if (env) {
        int val = std::atoi(env);
        if (val > 0) return val;
    }
    return 4096;
}

bool LiteRTLMOrchestrator::initMetadataIfNeeded(IGenerativeBackend& backend) const {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    if (metadata_initialized_) return true;

    auto* litert_backend = dynamic_cast<LiteRTLMBackend*>(&backend);
    if (!litert_backend) return false;

    const auto& meta = litert_backend->getMetadata();
    if (!meta.received) return false;

    jinja_template_          = meta.jinja_template;
    model_type_              = meta.model_type;
    max_context_length_      = (meta.max_context_length > 0)
                               ? meta.max_context_length : defaultContextLength();
    tool_call_delimiter_     = meta.tool_call_delimiter;
    tool_response_delimiter_ = meta.tool_response_delimiter;
    think_start_             = meta.think_start;
    think_end_               = meta.think_end;

    // Infer think tokens from jinja template if not provided by the worker
    // (Qwen3 and similar models embed <think>/<|think|> in their template).
    if (think_start_.empty() && !jinja_template_.empty()) {
        if (jinja_template_.find("<think>") != std::string::npos) {
            think_start_ = "<think>";
            think_end_   = "</think>";
        }
    }

    metadata_initialized_    = true;

    LOG_INFO("[LiteRTLMOrchestrator] Metadata initialized:"
             << " model_type=" << model_type_
             << " ctx=" << max_context_length_
             << " think_start='" << think_start_ << "'"
             << " think_end='" << think_end_ << "'");
    return true;
}

std::string LiteRTLMOrchestrator::renderPrompt(const json& messages,
                                                const json& tools,
                                                bool add_generation_prompt) const {
    if (!jinja_template_.empty()) {
        try {
            jinja2::Template tmpl;
            auto load_result = tmpl.Load(jinja_template_);
            if (!load_result) {
                LOG_WARN("[LiteRTLMOrchestrator] Failed to load Jinja2 template: "
                         << load_result.error().ToString());
            } else {
                jinja2::ValuesMap params;
                params["messages"] = jsonToJinjaValue(messages);
                params["add_generation_prompt"] = add_generation_prompt;
                params["enable_thinking"] = false;
                if (!tools.is_null() && tools.is_array() && !tools.empty()) {
                    params["tools"] = jsonToJinjaValue(tools);
                }

                auto render_result = tmpl.RenderAsString(params);
                if (render_result) {
                    return render_result.value();
                }
                LOG_WARN("[LiteRTLMOrchestrator] Failed to render Jinja2 template: "
                         << render_result.error().ToString());
            }
        } catch (const std::exception& e) {
            LOG_WARN("[LiteRTLMOrchestrator] Jinja2 rendering threw exception: "
                     << e.what());
        }
        // Fall through to the flat fallback formatter below on any failure —
        // this preserves the original safety net for malformed/unsupported
        // templates rather than propagating an exception up to the caller.
    }
    // Fallback: simple role: content format
    std::ostringstream oss;
    for (const auto& msg : messages)
        oss << msg.value("role","user") << ": " << msg.value("content","") << "\n";
    oss << "assistant:";
    return oss.str();
}

scheduler::GenerativeJobPtr LiteRTLMOrchestrator::createJob(
    scheduler::GenerativeJobContext context,
    scheduler::GenerativeCallbacks callbacks) const {
    json messages = json::array();
    if (context.caller.use_response_history &&
        context.caller.response_history.is_array()) {
        for (auto& message : context.caller.response_history) {
            messages.push_back(std::move(message));
        }
    }
    if (context.request.messages.is_array()) {
        for (auto& message : context.request.messages) {
            messages.push_back(std::move(message));
        }
    }

    json tools = std::move(context.request.tools).value_or(json::array());
    const int evicted = applyContextEviction(
        messages,
        tools,
        ModelConfigManager::getInstance().getContextSize(context.model_id));
    if (evicted > 0) {
        LOG_INFO("[LiteRTLMOrchestrator] Evicted " << evicted
                 << " messages while preparing model=" << context.model_id
                 << " — kv_invalidated=true");
    }

    scheduler::LiteRTLMPreparedRequest prepared;
    prepared.messages = std::move(messages);
    prepared.tools = std::move(tools);
    prepared.kv_invalidated = (evicted > 0);
    prepared.raw_prompt = context.request.raw_prompt;
    prepared.generation.max_tokens =
        context.request.max_completion_tokens.value_or([]() {
            const char* e = std::getenv("LITERT_LM_MAX_TOKENS");
            return (e && std::atoi(e) > 0) ? std::atoi(e) : 512;
        }());
    prepared.generation.temperature =
        context.request.temperature.value_or([]() {
            const char* e = std::getenv("LITERT_LM_DEFAULT_TEMPERATURE");
            return e ? std::stof(e) : 0.7f;
        }());
    prepared.generation.top_p = context.request.top_p.value_or([]() {
            const char* e = std::getenv("LITERT_LM_DEFAULT_TOP_P");
            return e ? std::stof(e) : 0.9f;
        }());
    prepared.generation.top_k = context.request.top_k.value_or([]() {
            const char* e = std::getenv("LITERT_LM_DEFAULT_TOP_K");
            return (e && std::atoi(e) > 0) ? std::atoi(e) : 40;
        }());
    prepared.generation.presence_penalty =
        context.request.presence_penalty.value_or(0.0f);
    prepared.generation.frequency_penalty =
        context.request.frequency_penalty.value_or(0.0f);
    prepared.generation.streaming = static_cast<bool>(callbacks.on_token);

    auto job = std::make_shared<scheduler::GenerativeJob>();
    job->response_id = context.caller.response_id.empty()
        ? context.job_id : std::move(context.caller.response_id);
    job->session_id = context.request.user.value_or(context.job_id);
    job->job_id = std::move(context.job_id);
    job->model_id = std::move(context.model_id);
    job->tool_chain_id = std::move(context.tool_chain_id);
    job->kind = context.kind;
    job->priority = context.priority;
    job->prepared = std::move(prepared);
    job->skip_post_turn_summarization =
        context.skip_post_turn_summarization;
    job->callbacks = std::move(callbacks);
    return job;
}

StandardResponse LiteRTLMOrchestrator::execute(
    scheduler::GenerativeJob& job,
    IGenerativeBackend& backend) const {
    auto* litert_backend = dynamic_cast<LiteRTLMBackend*>(&backend);
    if (!litert_backend) {
        throw std::runtime_error(
            "LiteRTLMOrchestrator requires LiteRTLMBackend");
    }

    const auto* prepared =
        std::get_if<scheduler::LiteRTLMPreparedRequest>(&job.prepared);
    if (!prepared) {
        throw std::runtime_error(
            "LiteRTLMOrchestrator received a non-LiteRT-LM prepared request");
    }

    // Lazy metadata init — populates jinja_template_, tool_call_delimiter_,
    // think_start_, think_end_, max_context_length_ from the backend.
    initMetadataIfNeeded(backend);

    const std::string event_id = generateEventId();
    const bool streaming = prepared->generation.streaming;

    // OIP raw_prompt bypass: if the caller supplied an already-formatted
    // prompt (OIP /generate with text_input), use it verbatim instead of
    // rendering the Jinja2 chat template from messages/tools.
    std::string prompt = prepared->raw_prompt.has_value()
        ? prepared->raw_prompt.value()
        : renderPrompt(prepared->messages, prepared->tools, true);

    std::string accumulated_text;
    std::string finish_reason = "stop";
    int completion_tokens = 0;
    bool inference_error = false;
    std::string error_message;

    // Reasoning routing: only meaningful when the model declares both a
    // think-start and think-end tag. ReasoningRouter's find()-based state
    // machine treats an empty tag as matching at every position, which would
    // infinite-loop on a non-empty buffer — so the router is only
    // constructed/used when both tags are non-empty, exactly mirroring the
    // guard the old hand-rolled splitting logic used.
    const bool has_thinking = !think_start_.empty() && !think_end_.empty();
    std::optional<ReasoningRouter> router;
    if (has_thinking) {
        router.emplace(job.session_id, job.model_id, think_start_, think_end_, -1);
    }
    // Tracks whether the first non-empty answer-channel chunk has been
    // emitted yet, so we can strip the leading "\n\r" that typically follows
    // </think> — matching the old manual splitter's trim behavior — without
    // trimming subsequent answer chunks.
    bool answer_started = false;
    // The old hand-rolled splitter withheld ALL streaming output — including
    // any pre-<think> preamble and reasoning content itself — until
    // </think> had been seen at least once in the raw stream (it searched
    // the running buffer for think_end_ and emitted nothing to the client
    // until found; if the model never emitted a thinking block at all,
    // nothing was ever streamed). Replicate that gating exactly: content
    // before the model has entered+exited a thinking block is never
    // forwarded, and reasoning_content chunks are never forwarded either.
    bool saw_reasoning = false;

    StreamChunk stream_chunk;
    stream_chunk.id    = job.session_id;
    stream_chunk.model = job.model_id;

    if (streaming && job.callbacks.on_token) {
        stream_chunk.role = "assistant";
        stream_chunk.content_delta = "";
        job.callbacks.on_token(stream_chunk);
        stream_chunk.role.reset();
    }

    backend.generateWithSession(
        event_id, job.session_id, prompt, streaming,
        prepared->generation.max_tokens,
        prepared->generation.temperature,
        prepared->generation.top_p,
        prepared->generation.top_k,
        prepared->generation.presence_penalty,
        prepared->generation.frequency_penalty,
        false,
        [&](const IPCTokenEvent& token) {
            if (token.content.find("__METADATA__:") == 0) return;
            accumulated_text += token.content;
            ++completion_tokens;
            if (job.isCancelled()) {
                backend.terminateWorker(true);
                return;
            }

            // Always feed the router (regardless of streaming/tool-call
            // state) so getThinkingContent()/getAnswerContent() reflect the
            // complete response afterward — matching the old blocking-path
            // behavior of splitting over the *entire* accumulated_text
            // unconditionally.
            std::vector<StreamChunk> chunks;
            if (has_thinking) {
                chunks = router->route(token.content);
            }

            if (!streaming || !job.callbacks.on_token) return;
            if (!tool_call_delimiter_.empty() &&
                    accumulated_text.find(tool_call_delimiter_) != std::string::npos)
                return;

            if (has_thinking) {
                // Only forward answer-channel chunks that arrive AFTER the
                // model has emitted at least one reasoning chunk — matches
                // the old manual buffering behavior exactly: it withheld
                // *everything* (including any pre-<think> preamble) until
                // </think> was found in the growing buffer, and reasoning
                // content itself was never forwarded to the client.
                for (auto& chunk : chunks) {
                    if (chunk.reasoning_content.has_value()) {
                        saw_reasoning = true;
                        continue;  // reasoning content never reaches the client
                    }
                    if (!chunk.content_delta.has_value()) continue;
                    if (!saw_reasoning) continue;  // suppress pre-think preamble
                    if (!answer_started) {
                        std::string& delta = chunk.content_delta.value();
                        size_t start = delta.find_first_not_of("\n\r");
                        delta = (start != std::string::npos)
                            ? delta.substr(start) : std::string();
                        if (delta.empty()) continue;  // still trimming
                        answer_started = true;
                    }
                    chunk.id = job.session_id;
                    chunk.model = job.model_id;
                    job.callbacks.on_token(chunk);
                }
            } else {
                stream_chunk.content_delta = token.content;
                stream_chunk.finish_reason.reset();
                job.callbacks.on_token(stream_chunk);
            }
        },
        [&](const IPCDoneEvent& done) {
            finish_reason = done.finish_reason.empty() ? "stop" : done.finish_reason;
        },
        [&](const IPCErrorEvent& error) {
            inference_error = true;
            error_message = error.message;
        },
        prepared->kv_invalidated);

    if (inference_error) {
        throw GenAIException(
            GenAIErrorCode::INFERENCE_FAILED,
            "LiteRT-LM inference failed: " + error_message,
            500);
    }

    // Extract thinking/answer content — via ReasoningRouter when the model
    // declares thinking tags (fed token-by-token above in both streaming and
    // blocking modes), otherwise the full accumulated text is the answer.
    std::string content_text;
    std::string reasoning_text;
    if (has_thinking) {
        reasoning_text = router->getThinkingContent();
        content_text = router->getAnswerContent();
        size_t first = content_text.find_first_not_of("\n\r \t");
        content_text = (first != std::string::npos)
            ? content_text.substr(first) : std::string();
    } else {
        content_text = accumulated_text;
    }

    StandardResponse response;
    response.id    = job.session_id;
    response.model = job.model_id;
    response.role  = "assistant";
    response.finish_reason = finish_reason;
    response.prompt_tokens = static_cast<int>(prompt.size() / 4);
    response.completion_tokens = completion_tokens;
    response.total_tokens = response.prompt_tokens + completion_tokens;
    if (!reasoning_text.empty()) response.reasoning_content = reasoning_text;

    if (!tool_call_delimiter_.empty() &&
            content_text.find(tool_call_delimiter_) != std::string::npos) {
        json tool_calls = parseToolCalls(content_text, tool_call_delimiter_);
        if (tool_calls.is_array() && !tool_calls.empty()) {
            response.tool_calls = std::move(tool_calls);
            response.finish_reason = "tool_calls";
            const std::string pre = extractPreToolContent(content_text, tool_call_delimiter_);
            if (!pre.empty()) response.content = pre;
        } else {
            response.content = content_text;
        }
    } else {
        response.content = content_text;
    }

    if (streaming && job.callbacks.on_token) {
        stream_chunk.content_delta.reset();
        stream_chunk.finish_reason = response.finish_reason;
        job.callbacks.on_token(stream_chunk);
    }

    return response;
}

int LiteRTLMOrchestrator::estimateTokens(
    const json& messages,
    const json& tools) const {
    const std::size_t characters = messages.dump().size() + tools.dump().size();
    return static_cast<int>(characters / 4) + 1;
}

int LiteRTLMOrchestrator::applyContextEviction(
    json& messages,
    const json& tools,
    int max_context_length) const {
    if (max_context_length <= 0) {
        return 0;
    }

    const int limit = static_cast<int>(
        max_context_length * compactionThreshold());
    int evicted = 0;
    while (messages.size() > 2 && estimateTokens(messages, tools) > limit) {
        const bool has_system =
            messages.front().value("role", "") == "system";
        const std::size_t index = has_system ? 1 : 0;
        if (index >= messages.size() - 1) {
            break;
        }
        messages.erase(index);
        ++evicted;
    }
    return evicted;
}

json LiteRTLMOrchestrator::parseToolCalls(
    const std::string& text,
    const std::string& delimiter) const {
    const std::size_t delimiter_position = text.find(delimiter);
    if (delimiter_position == std::string::npos) {
        return nullptr;
    }

    std::string payload = text.substr(
        delimiter_position + delimiter.size());
    while (!payload.empty() &&
           std::isspace(static_cast<unsigned char>(payload.front()))) {
        payload.erase(payload.begin());
    }

    try {
        std::size_t start = payload.find('[');
        if (start == std::string::npos) {
            start = payload.find('{');
        }
        if (start == std::string::npos) {
            return nullptr;
        }

        std::string candidate = payload.substr(start);
        const char opening = candidate.front();
        const char closing = opening == '[' ? ']' : '}';
        int depth = 0;
        std::size_t end = std::string::npos;
        for (std::size_t index = 0; index < candidate.size(); ++index) {
            if (candidate[index] == opening) {
                ++depth;
            } else if (candidate[index] == closing && --depth == 0) {
                end = index + 1;
                break;
            }
        }
        if (end == std::string::npos) {
            return nullptr;
        }

        json parsed = json::parse(candidate.substr(0, end));
        if (parsed.is_object()) {
            parsed = json::array({parsed});
        }
        if (!parsed.is_array()) {
            return nullptr;
        }

        for (auto& tool_call : parsed) {
            if (!tool_call.contains("id")) {
                tool_call["id"] = generateEventId();
            }
            if (!tool_call.contains("type")) {
                tool_call["type"] = "function";
            }
        }
        return parsed;
    } catch (const std::exception& error) {
        LOG_WARN("[LiteRTLMOrchestrator] Tool call parsing failed: "
                 << error.what());
        return nullptr;
    }
}

std::string LiteRTLMOrchestrator::extractPreToolContent(
    const std::string& text,
    const std::string& delimiter) const {
    const std::size_t delimiter_position = text.find(delimiter);
    if (delimiter_position == std::string::npos) {
        return text;
    }
    std::string content = text.substr(0, delimiter_position);
    while (!content.empty() &&
           std::isspace(static_cast<unsigned char>(content.back()))) {
        content.pop_back();
    }
    return content;
}
