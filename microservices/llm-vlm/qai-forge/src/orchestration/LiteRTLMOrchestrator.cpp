// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/orchestration/LiteRTLMOrchestrator.h"

#include "qai_forge/backend/LiteRTLMBackend.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/Logger.h"

#include <cctype>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

namespace {

std::string generateEventId() {
    static std::mt19937_64 rng(std::random_device{}());
    std::ostringstream oss;
    oss << "litert-lm-" << std::hex << std::setw(16) << std::setfill('0')
        << rng();
    return oss.str();
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
        if (jinja_template_.find("<|im_start|>") != std::string::npos) {
            std::ostringstream oss;
            for (const auto& msg : messages) {
                oss << "<|im_start|>" << msg.value("role","user") << "\n"
                    << msg.value("content","") << "<|im_end|>\n";
            }
            if (add_generation_prompt) {
                oss << "<|im_start|>assistant\n";
            }
            return oss.str();
        }
        if (jinja_template_.find("<|start_of_role|>") != std::string::npos) {
            std::ostringstream oss;
            bool has_system = !messages.empty() &&
                              messages[0].value("role","") == "system";
            if (!has_system) {
                oss << "<|start_of_role|>system<|end_of_role|>"
                    << "You are a helpful assistant."
                    << "<|end_of_text|>\n";
            }
            for (const auto& msg : messages) {
                oss << "<|start_of_role|>" << msg.value("role","user")
                    << "<|end_of_role|>" << msg.value("content","")
                    << "<|end_of_text|>\n";
            }
            if (add_generation_prompt)
                oss << "<|start_of_role|>assistant<|end_of_role|>";
            return oss.str();
        }
        json ctx = json::object();
        ctx["messages"] = messages;
        ctx["add_generation_prompt"] = add_generation_prompt;
        ctx["enable_thinking"] = false;
        if (!tools.is_null() && tools.is_array() && !tools.empty())
            ctx["tools"] = tools;
        try { return renderJinja(jinja_template_, ctx); } catch (...) {}
    }
    // Fallback: simple role: content format
    std::ostringstream oss;
    for (const auto& msg : messages)
        oss << msg.value("role","user") << ": " << msg.value("content","") << "\n";
    oss << "assistant:";
    return oss.str();
}

std::string LiteRTLMOrchestrator::renderJinja(const std::string& tmpl,
                                               const json& context) const {
    // Minimal Jinja2 renderer — handles the subset used by LLM chat templates.
    // Supports: {{ var }}, {% for x in y %}...{% endfor %},
    //           {% if x %}...{% elif x %}...{% else %}...{% endif %},
    //           whitespace control (- prefix/suffix), string filters.
    std::string result;
    result.reserve(tmpl.size() * 2);
    size_t pos = 0;

    auto lookup = [&](const std::string& key) -> json {
        if (context.contains(key)) return context[key];
        return json{};
    };

    // Simple single-pass renderer (handles top-level constructs)
    while (pos < tmpl.size()) {
        size_t tag_start = tmpl.find("{", pos);
        if (tag_start == std::string::npos) {
            result += tmpl.substr(pos);
            break;
        }
        if (tag_start + 1 >= tmpl.size()) {
            result += tmpl.substr(pos);
            break;
        }
        char next = tmpl[tag_start + 1];
        if (next == '{') {
            result += tmpl.substr(pos, tag_start - pos);
            size_t end = tmpl.find("}}", tag_start + 2);
            if (end == std::string::npos) { result += tmpl.substr(tag_start); break; }
            std::string expr = tmpl.substr(tag_start + 2, end - tag_start - 2);
            // trim
            size_t s = expr.find_first_not_of(" \t-");
            size_t e = expr.find_last_not_of(" \t-");
            if (s != std::string::npos) expr = expr.substr(s, e - s + 1);
            // handle simple var or var.field
            auto dot = expr.find('.');
            if (dot != std::string::npos) {
                std::string obj = expr.substr(0, dot);
                std::string field = expr.substr(dot + 1);
                auto v = lookup(obj);
                if (v.is_object() && v.contains(field)) {
                    auto fv = v[field];
                    if (fv.is_string()) result += fv.get<std::string>();
                    else result += fv.dump();
                }
            } else {
                auto v = lookup(expr);
                if (v.is_string()) result += v.get<std::string>();
                else if (!v.is_null()) result += v.dump();
            }
            pos = end + 2;
        } else if (next == '%') {
            result += tmpl.substr(pos, tag_start - pos);
            size_t end = tmpl.find("%}", tag_start + 2);
            if (end == std::string::npos) { result += tmpl.substr(tag_start); break; }
            pos = end + 2;
            // skip block tags for now (full Jinja is complex; fall through to fallback)
        } else {
            result += tmpl.substr(pos, tag_start - pos + 1);
            pos = tag_start + 1;
        }
    }
    return result;
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

    // Render messages to a flat prompt string for the session API worker.
    std::string prompt = renderPrompt(prepared->messages, prepared->tools, true);

    std::string accumulated_text;
    std::string finish_reason = "stop";
    int completion_tokens = 0;
    bool inference_error = false;
    std::string error_message;

    // Streaming: buffer tokens until </think> exits so clients never see
    // raw reasoning tokens.
    bool streaming_think_done = think_end_.empty();
    std::string streaming_buf;

    StreamChunk stream_chunk;
    stream_chunk.id    = job.session_id;
    stream_chunk.model = job.model_id;

    if (streaming && job.callbacks.on_token) {
        stream_chunk.role = "assistant";
        stream_chunk.content_delta = "";
        job.callbacks.on_token(stream_chunk);
        stream_chunk.role.reset();
    }

    backend.generate(
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
            if (!streaming || !job.callbacks.on_token) return;
            if (!tool_call_delimiter_.empty() &&
                    accumulated_text.find(tool_call_delimiter_) != std::string::npos)
                return;
            if (!streaming_think_done) {
                streaming_buf += token.content;
                auto pos = streaming_buf.find(think_end_);
                if (pos != std::string::npos) {
                    streaming_think_done = true;
                    std::string after = streaming_buf.substr(pos + think_end_.size());
                    size_t start = after.find_first_not_of("\n\r");
                    if (start != std::string::npos) after = after.substr(start);
                    if (!after.empty()) {
                        stream_chunk.content_delta = after;
                        stream_chunk.finish_reason.reset();
                        job.callbacks.on_token(stream_chunk);
                    }
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

    // Split think block from content
    std::string content_text = accumulated_text;
    std::string reasoning_text;
    if (!think_start_.empty() && !think_end_.empty()) {
        auto ts = accumulated_text.find(think_start_);
        auto te = accumulated_text.find(think_end_);
        if (ts != std::string::npos && te != std::string::npos && te > ts) {
            reasoning_text = accumulated_text.substr(
                ts + think_start_.size(), te - ts - think_start_.size());
            content_text = accumulated_text.substr(te + think_end_.size());
            size_t first = content_text.find_first_not_of("\n\r \t");
            if (first != std::string::npos) content_text = content_text.substr(first);
            else content_text.clear();
        }
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
