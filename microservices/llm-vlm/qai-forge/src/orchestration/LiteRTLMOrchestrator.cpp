// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/orchestration/LiteRTLMOrchestrator.h"

#include "qai_forge/adapters/ModelAdapterFactory.h"
#include "qai_forge/adapters/ToolCallJsonUtils.h"
#include "qai_forge/backend/LiteRTLMBackend.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/reasoning/ReasoningRouter.h"
#include "qai_forge/utils/Logger.h"
#include "qai_forge/utils/TokenEstimation.h"

#include "minja/minja.hpp"
#include "minja/chat-template.hpp"

#include <iomanip>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {

// nlohmann::json's .value(key, default) only falls back to `default` when
// the key is absent — if the key is present but explicitly null,
// .value<std::string>() throws json::type_error.302. This helper treats an
// explicit null the same as an absent key.
std::string getStringOrDefault(const json& obj,
                                const std::string& key,
                                const std::string& def = "") {
    if (!obj.is_object() || !obj.contains(key) || obj[key].is_null()) {
        return def;
    }
    const json& val = obj[key];
    return val.is_string() ? val.get<std::string>() : def;
}

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

    // Compile the chat template once via minja. bos_token/eos_token are not
    // currently surfaced by LiteRTLMMetadata — most HF chat templates only
    // reference {{ bos_token }}/{{ eos_token }} informationally (if at all),
    // and the worker's own tokenizer independently prepends/appends the
    // real BOS/EOS tokens around the rendered prompt text, so passing empty
    // strings here does not affect correctness of the generated prompt.
    if (!jinja_template_.empty()) {
        try {
            chat_template_ = std::make_unique<minja::chat_template>(
                jinja_template_, /*bos_token=*/"", /*eos_token=*/"");
        } catch (const std::exception& e) {
            LOG_WARN("[LiteRTLMOrchestrator] Failed to compile minja chat "
                     "template: " << e.what());
            chat_template_.reset();
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
    if (chat_template_) {
        try {
            json extra_context = json::object();
            extra_context["enable_thinking"] = false;

            json empty_tools = json::array();
            const json& tools_arg =
                (!tools.is_null() && tools.is_array() && !tools.empty())
                    ? tools : empty_tools;

            return chat_template_->apply(
                messages, tools_arg, add_generation_prompt, extra_context);
        } catch (const std::exception& e) {
            LOG_WARN("[LiteRTLMOrchestrator] minja rendering threw exception: "
                     << e.what());
        }
        // Fall through to the flat fallback formatter below on any failure —
        // this preserves the original safety net for malformed/unsupported
        // templates rather than propagating an exception up to the caller.
    }
    // Fallback: simple role: content format
    std::ostringstream oss;
    for (const auto& msg : messages)
        oss << getStringOrDefault(msg, "role", "user") << ": "
            << getStringOrDefault(msg, "content", "") << "\n";
    oss << "assistant:";
    return oss.str();
}

scheduler::GenerativeJobPtr LiteRTLMOrchestrator::createJob(
    scheduler::GenerativeJobContext context,
    scheduler::GenerativeCallbacks callbacks) const {
    json messages = std::move(context.request.messages);
    if (!messages.is_array()) {
        messages = json::array();
    }

    json tools = std::move(context.request.tools).value_or(json::array());
    const int evicted = qai_forge::utils::applyFrontEviction(
        messages,
        tools,
        ModelConfigManager::getInstance().getContextSize(context.model_id),
        compactionThreshold());
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
    job->session_id = context.execution_session_id.empty()
        ? context.job_id : std::move(context.execution_session_id);
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

    // Lazy metadata init — populates jinja_template_ (compiled into
    // chat_template_), think_start_, think_end_, max_context_length_ from
    // the backend.
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
        router.emplace(job.response_id, job.model_id, think_start_, think_end_, -1);
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
    stream_chunk.id    = job.response_id;
    stream_chunk.model = job.model_id;

    if (streaming && job.callbacks.on_token) {
        stream_chunk.role = "assistant";
        stream_chunk.content_delta = "";
        job.callbacks.on_token(stream_chunk);
        stream_chunk.role.reset();
    }

    // Resolved once per execute() call (not per-token) — the marker that,
    // once observed in the accumulated response text, indicates a tool-call
    // payload has started and per-token streaming must be suppressed until
    // the full response can be parsed via ModelAdapterFactory::parseToolCalls().
    // Empty for adapters that don't support tool calling or whose syntax
    // doesn't require buffering — in that case no suppression ever occurs.
    const std::string tool_call_start_marker =
        ModelAdapterFactory::getAdapter(job.model_id).toolCallStartMarker();

    // Reused across calls (cleared each token) to avoid a fresh heap
    // allocation on every streamed token when reasoning routing is active —
    // see ReasoningRouter::route()'s doc comment.
    std::vector<StreamChunk> reasoning_chunks_scratch;

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
            reasoning_chunks_scratch.clear();
            if (has_thinking) {
                router->route(token.content, reasoning_chunks_scratch);
            }
            std::vector<StreamChunk>& chunks = reasoning_chunks_scratch;

            if (!streaming || !job.callbacks.on_token) return;
            // Suppress raw per-token streaming ONLY once the adapter's
            // tool-call start marker has actually appeared in the
            // accumulated response text — NOT merely because the adapter is
            // tool-calling-capable in general. Gating on capability alone
            // would suppress 100% of streamed content for every response
            // from a tool-calling-capable model, even plain answers that
            // never contain a tool call. Once the marker is seen, tokens
            // are buffered until the full response can be parsed via
            // ModelAdapterFactory::parseToolCalls() below.
            if (!tool_call_start_marker.empty() &&
                    accumulated_text.find(tool_call_start_marker) != std::string::npos)
                return;

            if (has_thinking) {
                // Forward answer-channel content to the client as it
                // arrives; reasoning_content chunks are never forwarded
                // (they remain internal-only, accumulated into
                // reasoning_text for the final non-streaming response's
                // reasoning_content field).
                //
                // IMPORTANT: streaming must not be gated on having already
                // seen a reasoning chunk. Many models only optionally emit
                // a thinking block — for a plain answer with no thinking
                // block at all, gating on "saw_reasoning" would suppress
                // 100% of legitimate answer content for the entire
                // response. Only the leading "\n\r" that immediately
                // follows a real </think> boundary is trimmed (once).
                for (auto& chunk : chunks) {
                    if (chunk.reasoning_content.has_value()) {
                        saw_reasoning = true;
                        continue;  // reasoning content never reaches the client
                    }
                    if (!chunk.content_delta.has_value()) continue;
                    if (saw_reasoning && !answer_started) {
                        std::string& delta = chunk.content_delta.value();
                        size_t start = delta.find_first_not_of("\n\r");
                        delta = (start != std::string::npos)
                            ? delta.substr(start) : std::string();
                        answer_started = true;
                        if (delta.empty()) continue;  // fully-whitespace chunk trimmed away
                    }
                    chunk.id = job.response_id;
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
    response.id = job.response_id;
    response.model = job.model_id;
    response.role  = "assistant";
    response.finish_reason = finish_reason;
    response.prompt_tokens = static_cast<int>(prompt.size() / 4);
    response.completion_tokens = completion_tokens;
    response.total_tokens = response.prompt_tokens + completion_tokens;
    if (!reasoning_text.empty()) response.reasoning_content = reasoning_text;

    // Delegate tool-call parsing to the model-family adapter — mirrors
    // GenieOrchestrator's pattern exactly. Each model's own chat template
    // determines its tool-call syntax independent of backend, so
    // resolution is by model_id substring match via ModelAdapterFactory.
    // Passing prepared->tools enables case-insensitive tool-name
    // normalization/rejection and (for adapters that support it)
    // schema-guided recovery against the caller's declared tools.
    const auto& adapter = ModelAdapterFactory::getAdapter(job.model_id);
    const bool has_tools = prepared->tools.is_array() && !prepared->tools.empty();
    json tool_calls = adapter.parseToolCalls(content_text, prepared->tools);
    if (tool_calls.is_array() && !tool_calls.empty()) {
        response.tool_calls = std::move(tool_calls);
        response.finish_reason = "tool_calls";
        // content left unset, matching GenieOrchestrator — avoids leaking
        // raw tool-call syntax into message.content when tool_calls already
        // carries the structured, parsed call(s).
    } else if (has_tools &&
               ToolCallJsonUtils::isEmptyToolCallResponse(content_text)) {
        // An explicit empty tool_calls array is the model's way of
        // signalling "no tool needed" via the generic OpenAI-JSON
        // convention — suppress the raw control JSON so it never leaks
        // into the visible response content.
        response.content = "";
    } else if (has_tools && !tool_call_start_marker.empty() &&
               content_text.find(tool_call_start_marker) != std::string::npos) {
        // The model's tool-call start marker was observed but nothing
        // could be parsed out of the full response — surface loudly
        // instead of silently downgrading to a plain-text answer that
        // would actually contain raw tool-call syntax. Streaming already
        // withheld this content from the client via the marker-gated
        // suppression above.
        LOG_ERROR("[LiteRTLMOrchestrator] TOOL_CALL_PARSE_FAILED: model="
                  << job.model_id << " session=" << job.session_id
                  << " raw_response=\"" << content_text << "\"");
        throw GenAIException(
            GenAIErrorCode::INFERENCE_FAILED,
            "The model attempted a tool call but produced output that "
            "could not be parsed (tool_call_parse_failed)",
            502);
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
