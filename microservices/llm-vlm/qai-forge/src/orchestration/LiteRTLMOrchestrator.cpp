// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// LiteRTLMOrchestrator — Phase 4: IOrchestrator implementation for LiteRT-LM
//
// All chat intelligence lives here:
//   1. Jinja prompt rendering (lightweight Jinja2 subset for chat templates)
//   2. Tool call parsing (delimiter-based extraction)
//   3. Context overflow handling (sliding window eviction)
//   4. Universal post-inference KV reset (resetKvAsync)
//
// The worker subprocess (litert-lm-inference-worker) is intentionally dumb —
// it only runs prefill + decode_async on the raw prompt string.
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/orchestration/LiteRTLMOrchestrator.h"
#include "qai_forge/utils/Logger.h"

#include <sstream>
#include <algorithm>
#include <stdexcept>
#include <mutex>
#include <random>
#include <iomanip>

// ─────────────────────────────────────────────────────────────────────────────
// Lazy metadata initialization
//
// Called at the top of executeCore() on every invocation until metadata is
// successfully populated. Thread-safe via metadata_mutex_.
//
// The metadata is available only after ModelRuntime has called
// backend_->loadModel(), which starts the worker and triggers the METADATA
// IPC event. The orchestrator is constructed before loadModel() is called,
// so we cannot initialize metadata at construction time.
// ─────────────────────────────────────────────────────────────────────────────

bool LiteRTLMOrchestrator::initMetadataIfNeeded(IGenerativeBackend& backend) {
    std::lock_guard<std::mutex> lock(metadata_mutex_);
    if (metadata_initialized_) return true;

    auto* litert_backend = dynamic_cast<LiteRTLMBackend*>(&backend);
    if (!litert_backend) {
        LOG_WARN("[LiteRTLMOrchestrator] Backend is not LiteRTLMBackend — cannot init metadata");
        return false;
    }

    const auto& meta = litert_backend->getMetadata();
    if (!meta.received) {
        LOG_WARN("[LiteRTLMOrchestrator] Metadata not yet received from worker — using defaults");
        // Use defaults; will retry on next call
        return false;
    }

    jinja_template_          = meta.jinja_template;
    model_type_              = meta.model_type;
    max_context_length_      = (meta.max_context_length > 0) ? meta.max_context_length : 4096;
    tool_call_delimiter_     = meta.tool_call_delimiter;
    tool_response_delimiter_ = meta.tool_response_delimiter;
    metadata_initialized_    = true;

    LOG_INFO("[LiteRTLMOrchestrator] Metadata initialized lazily:"
             << " model_type=" << model_type_
             << " ctx=" << max_context_length_
             << " tool_call_delim='" << tool_call_delimiter_ << "'"
             << " jinja_template_len=" << jinja_template_.size());
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// IOrchestrator::execute()
// ─────────────────────────────────────────────────────────────────────────────

StandardResponse LiteRTLMOrchestrator::execute(
    const CreateChatCompletionRequest& request,
    const json& response_history,
    IGenerativeBackend& backend,
    OrchestratorStreamCallback callback,
    std::function<bool()> cancel)
{
    // Merge response_history into the request messages for context
    CreateChatCompletionRequest merged_request = request;
    if (!response_history.is_null() && response_history.is_array()
            && !response_history.empty()) {
        // Prepend history messages before the current request messages
        json combined = json::array();
        for (const auto& msg : response_history) {
            combined.push_back(msg);
        }
        for (const auto& msg : request.messages) {
            combined.push_back(msg);
        }
        merged_request.messages = combined;
    }

    if (callback) {
        return executeStreaming(merged_request, backend, callback,
                                cancel ? cancel : CancellationPredicate{});
    } else {
        return executeBlocking(merged_request, backend,
                               cancel ? cancel : CancellationPredicate{});
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// executeBlocking()
// ─────────────────────────────────────────────────────────────────────────────

StandardResponse LiteRTLMOrchestrator::executeBlocking(
    const CreateChatCompletionRequest& request,
    IGenerativeBackend& backend,
    CancellationPredicate cancel_requested)
{
    return executeCore(request, backend, nullptr, cancel_requested);
}

// ─────────────────────────────────────────────────────────────────────────────
// executeStreaming()
// ─────────────────────────────────────────────────────────────────────────────

StandardResponse LiteRTLMOrchestrator::executeStreaming(
    const CreateChatCompletionRequest& request,
    IGenerativeBackend& backend,
    OrchestratorStreamCallback callback,
    CancellationPredicate cancel_requested)
{
    return executeCore(request, backend, callback, cancel_requested);
}

// ─────────────────────────────────────────────────────────────────────────────
// executeCore() — Main inference pipeline
// ─────────────────────────────────────────────────────────────────────────────

StandardResponse LiteRTLMOrchestrator::executeCore(
    const CreateChatCompletionRequest& request,
    IGenerativeBackend& backend,
    OrchestratorStreamCallback callback,
    const CancellationPredicate& cancel_requested)
{
    // Lazy metadata initialization — runs once after ModelRuntime calls loadModel().
    // If metadata is not yet available (worker not started), proceed with defaults.
    initMetadataIfNeeded(backend);

    // Generate a unique event ID
    static std::mt19937_64 rng(std::random_device{}());
    std::ostringstream oss;
    oss << "litert-lm-" << std::hex << std::setw(16) << std::setfill('0') << rng();
    std::string event_id = oss.str();

    // Extract parameters
    int max_tokens    = request.max_completion_tokens.value_or(512);
    float temperature = request.temperature.value_or(0.7f);
    float top_p       = request.top_p.value_or(0.9f);
    int top_k         = request.top_k.value_or(40);
    float freq_pen    = request.frequency_penalty.value_or(0.0f);
    float pres_pen    = request.presence_penalty.value_or(0.0f);
    bool streaming    = request.stream && callback != nullptr;

    // Build message array with context eviction
    json messages = request.messages;
    json tools    = request.tools.value_or(json(nullptr));

    // Apply context eviction if needed
    int evicted = applyContextEviction(messages, tools);
    if (evicted > 0) {
        LOG_INFO("[LiteRTLMOrchestrator] Evicted " << evicted
                 << " messages to fit context window");
    }

    // Render prompt using Jinja template
    std::string prompt = renderPrompt(messages, tools, true);

    LOG_DEBUG("[LiteRTLMOrchestrator] executeCore event_id=" << event_id
              << " model=" << request.model
              << " prompt_len=" << prompt.size()
              << " max_tokens=" << max_tokens
              << " streaming=" << streaming);

    // Accumulate generated text for tool call detection
    std::string accumulated_text;
    std::string finish_reason = "stop";
    int completion_tokens = 0;

    // Streaming chunk for SSE delivery
    StreamChunk stream_chunk;
    stream_chunk.id    = event_id;
    stream_chunk.model = request.model;

    // Send role chunk first (streaming only)
    if (streaming && callback) {
        stream_chunk.role = "assistant";
        stream_chunk.content_delta = "";
        callback(stream_chunk);
        stream_chunk.role.reset();
    }

    // Run inference
    bool inference_error = false;
    std::string error_message;

    auto on_token = [&](const IPCTokenEvent& tok) {
        // Skip metadata probe tokens
        if (tok.content.find("__METADATA__:") == 0) return;

        accumulated_text += tok.content;
        completion_tokens++;

        if (streaming && callback) {
            // Check for tool call delimiter — stop streaming content at delimiter
            if (!tool_call_delimiter_.empty() &&
                    accumulated_text.find(tool_call_delimiter_) != std::string::npos) {
                // Don't stream tool call content — it will be parsed and returned
                // as structured tool_calls in the final response
                return;
            }

            stream_chunk.content_delta = tok.content;
            stream_chunk.finish_reason.reset();
            callback(stream_chunk);
        }

        if (cancel_requested && cancel_requested()) {
            backend.terminateWorker(true);
        }
    };

    auto on_done = [&](const IPCDoneEvent& done) {
        finish_reason = done.finish_reason.empty() ? "stop" : done.finish_reason;
    };

    auto on_error = [&](const IPCErrorEvent& err) {
        inference_error = true;
        error_message = err.message;
        LOG_ERROR("[LiteRTLMOrchestrator] Inference error: " << err.message);
    };

    backend.generate(
        event_id, prompt, streaming,
        max_tokens, temperature, top_p, top_k,
        pres_pen, freq_pen,
        false, // use_reasoning — not applicable for LiteRT-LM
        on_token, on_done, on_error);

    // Initiate eager background KV reset (overlaps with response delivery)
    backend.resetKvAsync();

    if (inference_error) {
        throw GenAIException(
            GenAIErrorCode::INFERENCE_FAILED,
            "LiteRT-LM inference failed: " + error_message,
            500);
    }

    // Build StandardResponse
    StandardResponse response;
    response.id    = event_id;
    response.model = request.model;
    response.role  = "assistant";
    response.finish_reason = finish_reason;
    response.completion_tokens = completion_tokens;

    // Check for tool calls in accumulated text
    if (!tool_call_delimiter_.empty() &&
            accumulated_text.find(tool_call_delimiter_) != std::string::npos) {
        // Parse tool calls
        json tool_calls = parseToolCalls(accumulated_text);
        if (!tool_calls.is_null() && tool_calls.is_array() && !tool_calls.empty()) {
            response.tool_calls = tool_calls;
            response.finish_reason = "tool_calls";

            // Extract content before the tool call delimiter
            std::string pre_content = extractPreToolContent(accumulated_text);
            if (!pre_content.empty()) {
                response.content = pre_content;
            }

            // Send final streaming chunk with tool_calls finish_reason
            if (streaming && callback) {
                stream_chunk.content_delta.reset();
                stream_chunk.finish_reason = "tool_calls";
                callback(stream_chunk);
            }
        } else {
            // Tool call parsing failed — return as plain text
            response.content = accumulated_text;
            if (streaming && callback) {
                stream_chunk.content_delta.reset();
                stream_chunk.finish_reason = finish_reason;
                callback(stream_chunk);
            }
        }
    } else {
        response.content = accumulated_text;

        // Send final streaming chunk
        if (streaming && callback) {
            stream_chunk.content_delta.reset();
            stream_chunk.finish_reason = finish_reason;
            callback(stream_chunk);
        }
    }

    return response;
}

// ─────────────────────────────────────────────────────────────────────────────
// renderPrompt() — Render messages to prompt string using Jinja template
// ─────────────────────────────────────────────────────────────────────────────

std::string LiteRTLMOrchestrator::renderPrompt(const json& messages,
                                                const json& tools,
                                                bool add_generation_prompt) const {
    if (jinja_template_.empty()) {
        // Fallback: simple role: content format
        std::ostringstream oss;
        for (const auto& msg : messages) {
            std::string role    = msg.value("role", "user");
            std::string content = msg.value("content", "");
            oss << role << ": " << content << "\n";
        }
        oss << "assistant:";
        return oss.str();
    }

    // Build Jinja context
    json context = json::object();
    context["messages"] = messages;
    context["add_generation_prompt"] = add_generation_prompt;

    if (!tools.is_null() && tools.is_array() && !tools.empty()) {
        context["tools"] = tools;
    }

    try {
        return renderJinja(jinja_template_, context);
    } catch (const std::exception& e) {
        LOG_WARN("[LiteRTLMOrchestrator] Jinja rendering failed: " << e.what()
                 << " — falling back to simple format");

        // Fallback
        std::ostringstream oss;
        for (const auto& msg : messages) {
            std::string role    = msg.value("role", "user");
            std::string content = msg.value("content", "");
            oss << role << ": " << content << "\n";
        }
        oss << "assistant:";
        return oss.str();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// renderJinja() — Lightweight Jinja2 renderer for chat templates
//
// Handles the subset of Jinja2 used by LLM chat templates:
//   - {{ variable }} and {{ object.field }} substitution
//   - {% for item in collection %} ... {% endfor %}
//   - {% if condition %} ... {% elif condition %} ... {% else %} ... {% endif %}
//   - {%- ... -%} whitespace control
//   - Filters: | tojson, | upper, | lower, | strip, | trim
//   - Special variables: loop.first, loop.last, loop.index
// ─────────────────────────────────────────────────────────────────────────────

std::string LiteRTLMOrchestrator::renderJinja(const std::string& tmpl,
                                               const json& context) const {
    std::string result;
    result.reserve(tmpl.size() * 2);

    // Helper: resolve a dotted path in the context (e.g. "message.role")
    auto resolve = [&](const std::string& expr, const json& local_ctx) -> json {
        // Strip whitespace
        std::string e = expr;
        while (!e.empty() && std::isspace(static_cast<unsigned char>(e.front()))) e.erase(e.begin());
        while (!e.empty() && std::isspace(static_cast<unsigned char>(e.back()))) e.pop_back();

        // Handle string literals
        if ((e.front() == '"' && e.back() == '"') ||
            (e.front() == '\'' && e.back() == '\'')) {
            return e.substr(1, e.size() - 2);
        }

        // Handle boolean literals
        if (e == "true")  return true;
        if (e == "false") return false;
        if (e == "none" || e == "None" || e == "null") return nullptr;

        // Dotted path resolution
        std::vector<std::string> parts;
        std::istringstream ss(e);
        std::string part;
        while (std::getline(ss, part, '.')) {
            parts.push_back(part);
        }

        // Try local context first, then global context
        const json* cur = nullptr;
        if (local_ctx.is_object() && local_ctx.contains(parts[0])) {
            cur = &local_ctx.at(parts[0]);
        } else if (context.is_object() && context.contains(parts[0])) {
            cur = &context.at(parts[0]);
        } else {
            return nullptr;
        }

        for (size_t i = 1; i < parts.size() && cur != nullptr; ++i) {
            if (cur->is_object() && cur->contains(parts[i])) {
                cur = &cur->at(parts[i]);
            } else {
                return nullptr;
            }
        }

        return cur ? *cur : json(nullptr);
    };

    // Helper: evaluate a simple condition
    auto evalCondition = [&](const std::string& cond, const json& local_ctx) -> bool {
        std::string c = cond;
        while (!c.empty() && std::isspace(static_cast<unsigned char>(c.front()))) c.erase(c.begin());
        while (!c.empty() && std::isspace(static_cast<unsigned char>(c.back()))) c.pop_back();

        // "not X"
        if (c.substr(0, 4) == "not ") {
            json val = resolve(c.substr(4), local_ctx);
            if (val.is_null()) return true;
            if (val.is_boolean()) return !val.get<bool>();
            if (val.is_string()) return val.get<std::string>().empty();
            if (val.is_array()) return val.empty();
            return false;
        }

        // "X is defined"
        if (c.size() > 11 && c.substr(c.size() - 11) == " is defined") {
            json val = resolve(c.substr(0, c.size() - 11), local_ctx);
            return !val.is_null();
        }

        // "X is not none" / "X is not None"
        if (c.size() > 12 && (c.substr(c.size() - 12) == " is not none" ||
                               c.substr(c.size() - 12) == " is not None")) {
            json val = resolve(c.substr(0, c.size() - 12), local_ctx);
            return !val.is_null();
        }

        // "X == 'value'"
        size_t eq_pos = c.find(" == ");
        if (eq_pos != std::string::npos) {
            json lhs = resolve(c.substr(0, eq_pos), local_ctx);
            json rhs = resolve(c.substr(eq_pos + 4), local_ctx);
            return lhs == rhs;
        }

        // "X != 'value'"
        size_t neq_pos = c.find(" != ");
        if (neq_pos != std::string::npos) {
            json lhs = resolve(c.substr(0, neq_pos), local_ctx);
            json rhs = resolve(c.substr(neq_pos + 4), local_ctx);
            return lhs != rhs;
        }

        // Simple truthiness
        json val = resolve(c, local_ctx);
        if (val.is_null()) return false;
        if (val.is_boolean()) return val.get<bool>();
        if (val.is_string()) return !val.get<std::string>().empty();
        if (val.is_array()) return !val.empty();
        if (val.is_number()) return val.get<double>() != 0.0;
        return true;
    };

    // Helper: apply a filter to a value
    auto applyFilter = [&](const json& val, const std::string& filter) -> std::string {
        std::string s;
        if (val.is_string()) {
            s = val.get<std::string>();
        } else if (!val.is_null()) {
            s = val.dump();
        }

        if (filter == "tojson") return val.dump();
        if (filter == "upper") {
            for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return s;
        }
        if (filter == "lower") {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }
        if (filter == "strip" || filter == "trim") {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
            return s;
        }
        return s;
    };

    // Recursive rendering function
    std::function<std::string(const std::string&, const json&)> render;
    render = [&](const std::string& t, const json& local_ctx) -> std::string {
        std::string out;
        out.reserve(t.size());
        size_t p = 0;
        const size_t n = t.size();

        while (p < n) {
            // Find next {{ or {%
            size_t expr_pos = t.find("{{", p);
            size_t tag_pos  = t.find("{%", p);

            size_t next = std::min(expr_pos, tag_pos);
            if (next == std::string::npos) {
                out += t.substr(p);
                break;
            }

            // Append literal text before the tag
            out += t.substr(p, next - p);
            p = next;

            if (p == expr_pos && (tag_pos == std::string::npos || expr_pos <= tag_pos)) {
                // {{ expression }}
                size_t end = t.find("}}", p + 2);
                if (end == std::string::npos) { out += t.substr(p); break; }

                std::string expr = t.substr(p + 2, end - p - 2);
                // Strip whitespace control
                while (!expr.empty() && (expr.front() == '-' || std::isspace(static_cast<unsigned char>(expr.front()))))
                    expr.erase(expr.begin());
                while (!expr.empty() && (expr.back() == '-' || std::isspace(static_cast<unsigned char>(expr.back()))))
                    expr.pop_back();

                // Handle filters (pipe)
                std::string filter;
                size_t pipe = expr.find(" | ");
                if (pipe != std::string::npos) {
                    filter = expr.substr(pipe + 3);
                    expr   = expr.substr(0, pipe);
                    while (!filter.empty() && std::isspace(static_cast<unsigned char>(filter.front()))) filter.erase(filter.begin());
                    while (!filter.empty() && std::isspace(static_cast<unsigned char>(filter.back()))) filter.pop_back();
                }

                json val = resolve(expr, local_ctx);
                std::string rendered;
                if (!filter.empty()) {
                    rendered = applyFilter(val, filter);
                } else if (val.is_string()) {
                    rendered = val.get<std::string>();
                } else if (!val.is_null()) {
                    rendered = val.dump();
                }
                out += rendered;
                p = end + 2;

            } else {
                // {% tag %}
                bool strip_before = (p + 2 < n && t[p + 2] == '-');
                size_t tag_start = p + 2 + (strip_before ? 1 : 0);
                size_t end = t.find("%}", p + 2);
                if (end == std::string::npos) { out += t.substr(p); break; }

                bool strip_after = (end > 0 && t[end - 1] == '-');
                std::string tag_content = t.substr(tag_start, end - tag_start - (strip_after ? 1 : 0));

                // Strip whitespace from tag content
                while (!tag_content.empty() && std::isspace(static_cast<unsigned char>(tag_content.front()))) tag_content.erase(tag_content.begin());
                while (!tag_content.empty() && std::isspace(static_cast<unsigned char>(tag_content.back()))) tag_content.pop_back();

                // Strip trailing whitespace/newline from output if strip_before
                if (strip_before) {
                    while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
                }

                p = end + 2;

                // Strip leading whitespace/newline after tag if strip_after
                if (strip_after && p < n && (t[p] == '\n' || t[p] == '\r')) {
                    if (t[p] == '\r' && p + 1 < n && t[p + 1] == '\n') p += 2;
                    else p++;
                }

                if (tag_content.substr(0, 3) == "for") {
                    // {% for item in collection %}
                    // Parse: "for VAR in EXPR"
                    std::string for_body = tag_content.substr(4);
                    size_t in_pos = for_body.find(" in ");
                    if (in_pos == std::string::npos) continue;

                    std::string var_name = for_body.substr(0, in_pos);
                    while (!var_name.empty() && std::isspace(static_cast<unsigned char>(var_name.front()))) var_name.erase(var_name.begin());
                    while (!var_name.empty() && std::isspace(static_cast<unsigned char>(var_name.back()))) var_name.pop_back();

                    std::string coll_expr = for_body.substr(in_pos + 4);
                    while (!coll_expr.empty() && std::isspace(static_cast<unsigned char>(coll_expr.front()))) coll_expr.erase(coll_expr.begin());
                    while (!coll_expr.empty() && std::isspace(static_cast<unsigned char>(coll_expr.back()))) coll_expr.pop_back();

                    // Find matching {% endfor %}
                    int depth = 1;
                    size_t body_start = p;
                    size_t body_end = p;
                    while (body_end < n && depth > 0) {
                        size_t next_tag = t.find("{%", body_end);
                        if (next_tag == std::string::npos) break;
                        size_t next_end = t.find("%}", next_tag);
                        if (next_end == std::string::npos) break;
                        std::string inner = t.substr(next_tag + 2, next_end - next_tag - 2);
                        while (!inner.empty() && (inner.front() == '-' || std::isspace(static_cast<unsigned char>(inner.front())))) inner.erase(inner.begin());
                        while (!inner.empty() && (inner.back() == '-' || std::isspace(static_cast<unsigned char>(inner.back())))) inner.pop_back();
                        if (inner.substr(0, 3) == "for") depth++;
                        else if (inner == "endfor") { depth--; if (depth == 0) { body_end = next_tag; p = next_end + 2; break; } }
                        body_end = next_end + 2;
                    }

                    std::string loop_body = t.substr(body_start, body_end - body_start);
                    json collection = resolve(coll_expr, local_ctx);

                    if (collection.is_array()) {
                        size_t sz = collection.size();
                        for (size_t i = 0; i < sz; ++i) {
                            json loop_ctx = local_ctx;
                            loop_ctx[var_name] = collection[i];
                            loop_ctx["loop"] = {
                                {"index",  static_cast<int>(i + 1)},
                                {"index0", static_cast<int>(i)},
                                {"first",  i == 0},
                                {"last",   i == sz - 1},
                                {"length", static_cast<int>(sz)}
                            };
                            out += render(loop_body, loop_ctx);
                        }
                    }

                } else if (tag_content.substr(0, 2) == "if") {
                    // {% if condition %} ... {% elif %} ... {% else %} ... {% endif %}
                    std::string cond_expr = tag_content.substr(3);

                    // Find the matching endif, collecting elif/else branches
                    struct Branch { std::string condition; std::string body; };
                    std::vector<Branch> branches;
                    branches.push_back({cond_expr, ""});

                    int depth = 1;
                    size_t branch_start = p;
                    size_t scan = p;

                    while (scan < n && depth > 0) {
                        size_t next_tag = t.find("{%", scan);
                        if (next_tag == std::string::npos) break;
                        size_t next_end = t.find("%}", next_tag);
                        if (next_end == std::string::npos) break;

                        std::string inner = t.substr(next_tag + 2, next_end - next_tag - 2);
                        while (!inner.empty() && (inner.front() == '-' || std::isspace(static_cast<unsigned char>(inner.front())))) inner.erase(inner.begin());
                        while (!inner.empty() && (inner.back() == '-' || std::isspace(static_cast<unsigned char>(inner.back())))) inner.pop_back();

                        if (inner.substr(0, 2) == "if") {
                            depth++;
                        } else if (depth == 1 && inner.substr(0, 4) == "elif") {
                            branches.back().body = t.substr(branch_start, next_tag - branch_start);
                            branches.push_back({inner.substr(5), ""});
                            branch_start = next_end + 2;
                        } else if (depth == 1 && inner == "else") {
                            branches.back().body = t.substr(branch_start, next_tag - branch_start);
                            branches.push_back({"__else__", ""});
                            branch_start = next_end + 2;
                        } else if (inner == "endif") {
                            depth--;
                            if (depth == 0) {
                                branches.back().body = t.substr(branch_start, next_tag - branch_start);
                                p = next_end + 2;
                                break;
                            }
                        }
                        scan = next_end + 2;
                    }

                    // Evaluate branches
                    for (const auto& branch : branches) {
                        bool taken = (branch.condition == "__else__") ||
                                     evalCondition(branch.condition, local_ctx);
                        if (taken) {
                            out += render(branch.body, local_ctx);
                            break;
                        }
                    }

                } else if (tag_content == "endfor" || tag_content == "endif" ||
                           tag_content == "else" || tag_content.substr(0, 4) == "elif") {
                    // These are handled by the for/if parsers above
                    // If we encounter them here, it's a parse error — skip
                } else if (tag_content.substr(0, 3) == "set") {
                    // {% set var = value %} — simple variable assignment
                    // Not modifying local_ctx here (immutable in this impl)
                    // This is a limitation — set is rarely needed in chat templates
                }
                // Other tags (macro, block, extends, etc.) are not supported
            }
        }

        return out;
    };

    return render(tmpl, json::object());
}

// ─────────────────────────────────────────────────────────────────────────────
// estimateTokens()
// ─────────────────────────────────────────────────────────────────────────────

int LiteRTLMOrchestrator::estimateTokens(const std::string& text) const {
    // BPE approximation: ~4 characters per token
    return static_cast<int>(text.size() / 4) + 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// applyContextEviction()
// ─────────────────────────────────────────────────────────────────────────────

int LiteRTLMOrchestrator::applyContextEviction(json& messages, const json& tools) const {
    if (max_context_length_ <= 0) return 0;

    int eviction_limit = static_cast<int>(max_context_length_ * kCompactionThreshold);
    int evicted = 0;

    while (messages.size() > 2) {
        std::string prompt = renderPrompt(messages, tools, true);
        int tokens = estimateTokens(prompt);

        if (tokens <= eviction_limit) break;

        // Find the oldest non-system message to evict
        // Keep: index 0 (system message if present), last message (current user turn)
        size_t evict_idx = 0;
        bool has_system = (!messages.empty() &&
                           messages[0].value("role", "") == "system");
        evict_idx = has_system ? 1 : 0;

        if (evict_idx >= messages.size() - 1) {
            // Can't evict further without losing the current turn
            LOG_WARN("[LiteRTLMOrchestrator] Cannot evict further — context may be truncated");
            break;
        }

        LOG_DEBUG("[LiteRTLMOrchestrator] Evicting message at index " << evict_idx
                  << " (role=" << messages[evict_idx].value("role", "?") << ")");
        messages.erase(evict_idx);
        evicted++;
    }

    return evicted;
}

// ─────────────────────────────────────────────────────────────────────────────
// hasToolCallDelimiter()
// ─────────────────────────────────────────────────────────────────────────────

bool LiteRTLMOrchestrator::hasToolCallDelimiter(const std::string& text) const {
    if (tool_call_delimiter_.empty()) return false;
    return text.find(tool_call_delimiter_) != std::string::npos;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseToolCalls()
// ─────────────────────────────────────────────────────────────────────────────

json LiteRTLMOrchestrator::parseToolCalls(const std::string& text) const {
    if (tool_call_delimiter_.empty()) return nullptr;

    size_t delim_pos = text.find(tool_call_delimiter_);
    if (delim_pos == std::string::npos) return nullptr;

    // Extract text after the delimiter
    std::string after_delim = text.substr(delim_pos + tool_call_delimiter_.size());

    // Strip whitespace
    while (!after_delim.empty() && std::isspace(static_cast<unsigned char>(after_delim.front())))
        after_delim.erase(after_delim.begin());

    // Try to parse as JSON array of tool calls
    try {
        // Find the JSON object/array
        size_t json_start = after_delim.find('[');
        if (json_start == std::string::npos) {
            json_start = after_delim.find('{');
        }
        if (json_start == std::string::npos) return nullptr;

        std::string json_str = after_delim.substr(json_start);

        // Find matching closing bracket
        int depth = 0;
        char open_char = json_str[0];
        char close_char = (open_char == '[') ? ']' : '}';
        size_t json_end = std::string::npos;

        for (size_t i = 0; i < json_str.size(); ++i) {
            if (json_str[i] == open_char) depth++;
            else if (json_str[i] == close_char) {
                depth--;
                if (depth == 0) { json_end = i + 1; break; }
            }
        }

        if (json_end == std::string::npos) return nullptr;

        json parsed = json::parse(json_str.substr(0, json_end));

        // Normalize to array
        if (parsed.is_object()) {
            // Single tool call — wrap in array and add OpenAI-compatible fields
            json tool_call = parsed;
            if (!tool_call.contains("id")) {
                static std::mt19937_64 rng(std::random_device{}());
                std::ostringstream oss;
                oss << "call_" << std::hex << std::setw(8) << std::setfill('0') << rng();
                tool_call["id"] = oss.str();
            }
            if (!tool_call.contains("type")) {
                tool_call["type"] = "function";
            }
            return json::array({tool_call});
        } else if (parsed.is_array()) {
            // Multiple tool calls — add IDs if missing
            json result = json::array();
            for (auto& tc : parsed) {
                if (!tc.contains("id")) {
                    static std::mt19937_64 rng(std::random_device{}());
                    std::ostringstream oss;
                    oss << "call_" << std::hex << std::setw(8) << std::setfill('0') << rng();
                    tc["id"] = oss.str();
                }
                if (!tc.contains("type")) {
                    tc["type"] = "function";
                }
                result.push_back(tc);
            }
            return result;
        }
    } catch (const std::exception& e) {
        LOG_WARN("[LiteRTLMOrchestrator] Tool call parsing failed: " << e.what());
    }

    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// extractPreToolContent()
// ─────────────────────────────────────────────────────────────────────────────

std::string LiteRTLMOrchestrator::extractPreToolContent(const std::string& text) const {
    if (tool_call_delimiter_.empty()) return text;

    size_t delim_pos = text.find(tool_call_delimiter_);
    if (delim_pos == std::string::npos) return text;

    std::string pre = text.substr(0, delim_pos);

    // Strip trailing whitespace
    while (!pre.empty() && std::isspace(static_cast<unsigned char>(pre.back())))
        pre.pop_back();

    return pre;
}
