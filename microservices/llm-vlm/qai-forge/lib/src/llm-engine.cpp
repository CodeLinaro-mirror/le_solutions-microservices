// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// llm-engine.cpp — Layer 4 LLM Engine Implementation
//
// Replaces llm-service.cpp + llm-interface.cpp.
//
// Key simplifications vs. the old implementation:
//   1. No Response/Message/Choices structs — tokens are plain std::string.
//   2. No streaming/non-streaming split — always streams via TokenCallback.
//      Layer 3 accumulates tokens when the upstream request is non-streaming.
//   3. No C interface (llm-interface.h) — Layer 3 uses LlmEngine directly.
//   4. Internal GenIE handles are hidden in LlmEngine::Impl (Pimpl idiom).
// ─────────────────────────────────────────────────────────────────────────────

#include "llm-engine.hpp"
#include "GenieCommon.h"
#include "GenieDialog.h"
#include "GenieLog.h"
#include "GenieProfile.h"
#include "GenieSampler.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers (not exposed in header)
// ─────────────────────────────────────────────────────────────────────────────
namespace {

// ── GenIE log callback ────────────────────────────────────────────────────────
static void genieLogCallback(
    const _GenieLog_Handle_t* /*handle*/,
    const char* format,
    GenieLog_Level_t level,
    long unsigned int /*timestampOrSize*/,
    va_list args) {
    const char* levelStr =
        (level == GENIE_LOG_LEVEL_ERROR)   ? "ERROR"   :
        (level == GENIE_LOG_LEVEL_WARN)    ? "WARN"    :
        (level == GENIE_LOG_LEVEL_VERBOSE) ? "VERBOSE" : "INFO";
    char buf[4096];
    if (format) vsnprintf(buf, sizeof(buf), format, args);
    else        buf[0] = '\0';
    std::fprintf(stdout, "[GenIE-LLM] [%s] %s\n", levelStr, buf);
    std::fflush(stdout);
}

static GenieLog_Level_t parseLogLevel() {
    const char* env = std::getenv("LOG_LEVEL");
    if (!env) return GENIE_LOG_LEVEL_INFO;
    std::string s(env);
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    if (s == "error" || s == "err")      return GENIE_LOG_LEVEL_ERROR;
    if (s == "warn"  || s == "warning")  return GENIE_LOG_LEVEL_WARN;
    if (s == "verbose"|| s == "debug")   return GENIE_LOG_LEVEL_VERBOSE;
    return GENIE_LOG_LEVEL_INFO;
}

// ── Think-block filter ────────────────────────────────────────────────────────
// Strips <think>...</think> blocks from the token stream.
// State is carried across callback invocations via ThinkFilterState.

struct ThinkFilterState {
    bool        in_think   = false;
    std::string carry;        // partial marker buffered at end of chunk
    std::string active_end;   // the closing marker we're looking for
};

struct MarkerPair { std::string begin, end; };
static const std::vector<MarkerPair> kThinkMarkers = {{"<think>", "</think>"}};

static size_t longestSuffixPrefix(const std::string& data, const std::string& token) {
    const size_t max_len = std::min(token.size() - 1, data.size());
    for (size_t len = max_len; len > 0; --len)
        if (data.compare(data.size() - len, len, token, 0, len) == 0) return len;
    return 0;
}

static size_t longestSuffixPrefixAny(const std::string& data) {
    size_t best = 0;
    for (const auto& m : kThinkMarkers) {
        size_t s = longestSuffixPrefix(data, m.begin);
        if (s > best) best = s;
    }
    return best;
}

static std::string filterThinkBlocks(const std::string& chunk, ThinkFilterState& st) {
    std::string data = st.carry + chunk;
    st.carry.clear();
    std::string out;
    size_t pos = 0;

    while (pos < data.size()) {
        if (st.in_think) {
            size_t end_pos = data.find(st.active_end, pos);
            if (end_pos == std::string::npos) {
                size_t suffix = longestSuffixPrefix(data, st.active_end);
                if (suffix > 0) st.carry = data.substr(data.size() - suffix);
                return out;
            }
            pos = end_pos + st.active_end.size();
            st.in_think = false;
            st.active_end.clear();
            continue;
        }

        // Find next begin marker
        size_t best_pos = std::string::npos;
        const MarkerPair* best_marker = nullptr;
        for (const auto& m : kThinkMarkers) {
            size_t p = data.find(m.begin, pos);
            if (p != std::string::npos && (best_marker == nullptr || p < best_pos)) {
                best_pos = p;
                best_marker = &m;
            }
        }

        if (!best_marker) {
            size_t suffix = longestSuffixPrefixAny(data);
            if (suffix > 0) {
                out.append(data, pos, data.size() - pos - suffix);
                st.carry = data.substr(data.size() - suffix);
            } else {
                out.append(data, pos, std::string::npos);
            }
            return out;
        }

        out.append(data, pos, best_pos - pos);
        pos = best_pos + best_marker->begin.size();
        st.in_think = true;
        st.active_end = best_marker->end;
    }
    return out;
}

// ── Callback context ──────────────────────────────────────────────────────────
// Threaded through GenieDialog_query's userData pointer.
struct CallbackCtx {
    TokenCallback*   callback;
    bool             bypass_think_filter;
    ThinkFilterState think_state;
};

static void dialogCallback(const char* responseStr,
                            GenieDialog_SentenceCode_t sentenceCode,
                            const void* userData) {
    auto* ctx = static_cast<CallbackCtx*>(const_cast<void*>(userData));
    if (!ctx || !ctx->callback) return;

    std::string token;
    if (responseStr && responseStr[0] != '\0') {
        token = ctx->bypass_think_filter
            ? std::string(responseStr)
            : filterThinkBlocks(responseStr, ctx->think_state);
    }

    if (sentenceCode == GENIE_DIALOG_SENTENCE_END) {
        // Flush any buffered carry on end-of-sentence
        if (!ctx->bypass_think_filter && !ctx->think_state.carry.empty()) {
            token += ctx->think_state.carry;
            ctx->think_state.carry.clear();
        }
        // Final call — emit with finish_reason
        (*ctx->callback)(token, "stop");
    } else if (!token.empty()) {
        // Intermediate token — empty finish_reason
        (*ctx->callback)(token, "");
    }
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// LlmEngine::Impl — owns all GenIE handles
// ─────────────────────────────────────────────────────────────────────────────
struct LlmEngine::Impl {
    GenieLog_Handle_t     log_handle     = nullptr;
    GenieProfile_Handle_t profile_handle = nullptr;
    GenieDialog_Handle_t  dialog_handle  = nullptr;
    GenieSampler_Handle_t sampler_handle = nullptr;  // lazy-initialized
    std::string           sampler_config_path;
    std::string           model_id;

    ~Impl() {
        if (dialog_handle)  GenieDialog_free(dialog_handle);
        if (profile_handle) GenieProfile_free(profile_handle);
        if (log_handle)     GenieLog_free(log_handle);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// LlmEngine — constructor
// ─────────────────────────────────────────────────────────────────────────────
LlmEngine::LlmEngine(const std::string& model_id,
                     const std::string& config_path,
                     const std::string& sampler_config_path)
    : m_impl(std::make_unique<Impl>()) {
    m_impl->model_id = model_id;
    m_impl->sampler_config_path = sampler_config_path.empty() ? "sampler.json" : sampler_config_path;

    // ── Log ───────────────────────────────────────────────────────────────────
    {
        int32_t status = GenieLog_create(nullptr, genieLogCallback, parseLogLevel(),
                                          &m_impl->log_handle);
        if (status != GENIE_STATUS_SUCCESS || !m_impl->log_handle)
            throw std::runtime_error("[LlmEngine] Failed to create log handle");
    }

    // ── Profile ───────────────────────────────────────────────────────────────
    {
        int32_t status = GenieProfile_create(nullptr, &m_impl->profile_handle);
        if (status != GENIE_STATUS_SUCCESS || !m_impl->profile_handle)
            throw std::runtime_error("[LlmEngine] Failed to create profile handle");
    }

    // ── Read config JSON ──────────────────────────────────────────────────────
    std::ifstream f(config_path);
    if (!f.is_open())
        throw std::runtime_error("[LlmEngine] Failed to open config: " + config_path);
    std::string config_json((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());

    // ── Dialog config ─────────────────────────────────────────────────────────
    GenieDialogConfig_Handle_t dialog_config = nullptr;
    {
        int32_t status = GenieDialogConfig_createFromJson(config_json.c_str(), &dialog_config);
        if (status != GENIE_STATUS_SUCCESS || !dialog_config)
            throw std::runtime_error("[LlmEngine] Failed to create dialog config");
        GenieDialogConfig_bindProfiler(dialog_config, m_impl->profile_handle);
        GenieDialogConfig_bindLogger(dialog_config, m_impl->log_handle);
    }

    // ── Dialog ────────────────────────────────────────────────────────────────
    {
        int32_t status = GenieDialog_create(dialog_config, &m_impl->dialog_handle);
        GenieDialogConfig_free(dialog_config);
        if (status != GENIE_STATUS_SUCCESS || !m_impl->dialog_handle)
            throw std::runtime_error("[LlmEngine] Failed to create dialog");
    }

    std::cout << "[LlmEngine] Model loaded: " << model_id << std::endl;
}

LlmEngine::~LlmEngine() = default;

// ─────────────────────────────────────────────────────────────────────────────
// LlmEngine::generate
// ─────────────────────────────────────────────────────────────────────────────
void LlmEngine::generate(const std::string& prompt,
                          const GenerationConfig& config,
                          TokenCallback callback) {
    // ── Apply sampler config if non-default ───────────────────────────────────
    if (config.temperature != 1.0f || config.top_p != 1.0f || config.top_k > 0 ||
        config.presence_penalty != 0.0f || config.frequency_penalty != 0.0f) {
        // Lazy-initialize sampler handle
        if (!m_impl->sampler_handle) {
            int32_t status = GenieDialog_getSampler(m_impl->dialog_handle,
                                                     &m_impl->sampler_handle);
            if (status != GENIE_STATUS_SUCCESS)
                throw std::runtime_error("[LlmEngine] Failed to get sampler");
        }

        std::ifstream sc_file(m_impl->sampler_config_path);
        std::string sc_json;
        if (sc_file.is_open())
            sc_json.assign(std::istreambuf_iterator<char>(sc_file), {});

        GenieSamplerConfig_Handle_t sc_handle = nullptr;
        if (!sc_json.empty())
            GenieSamplerConfig_createFromJson(sc_json.c_str(), &sc_handle);

        if (sc_handle) {
            if (config.temperature != 1.0f)
                GenieSamplerConfig_setParam(sc_handle, "temp",
                    std::to_string(config.temperature).c_str());
            if (config.top_p != 1.0f)
                GenieSamplerConfig_setParam(sc_handle, "top-p",
                    std::to_string(config.top_p).c_str());
            if (config.top_k > 0)
                GenieSamplerConfig_setParam(sc_handle, "top-k",
                    std::to_string(config.top_k).c_str());
            if (config.presence_penalty != 0.0f)
                GenieSamplerConfig_setParam(sc_handle, "presence-penalty",
                    std::to_string(config.presence_penalty).c_str());
            if (config.frequency_penalty != 0.0f)
                GenieSamplerConfig_setParam(sc_handle, "frequency-penalty",
                    std::to_string(config.frequency_penalty).c_str());
            GenieSampler_applyConfig(m_impl->sampler_handle, sc_handle);
            GenieSamplerConfig_free(sc_handle);
        }
    }

    // ── Set max tokens ────────────────────────────────────────────────────────
    if (config.max_tokens > 0)
        GenieDialog_setMaxNumTokens(m_impl->dialog_handle, config.max_tokens);

    // ── Run inference ─────────────────────────────────────────────────────────
    CallbackCtx ctx;
    ctx.callback = &callback;
    ctx.bypass_think_filter = config.bypass_think_filter;

    int32_t status = GenieDialog_query(
        m_impl->dialog_handle,
        prompt.c_str(),
        GENIE_DIALOG_SENTENCE_COMPLETE,
        dialogCallback,
        &ctx);

    if (status == GENIE_STATUS_WARNING_ABORTED) {
        std::cout << "[LlmEngine] Query aborted\n";
    } else if (status != GENIE_STATUS_SUCCESS &&
               status != GENIE_STATUS_WARNING_CONTEXT_EXCEEDED) {
        throw std::runtime_error("[LlmEngine] GenieDialog_query failed: " +
                                  std::to_string(status));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// LlmEngine — KV cache & reset
// ─────────────────────────────────────────────────────────────────────────────
void LlmEngine::reset() {
    int32_t status = GenieDialog_reset(m_impl->dialog_handle);
    if (status != GENIE_STATUS_SUCCESS)
        throw std::runtime_error("[LlmEngine] Failed to reset dialog");
}

void LlmEngine::save_kv(const std::string& name) {
    int32_t status = GenieDialog_save(m_impl->dialog_handle, name.c_str());
    if (status != GENIE_STATUS_SUCCESS)
        throw std::runtime_error("[LlmEngine] Failed to save KV cache: " + name);
}

void LlmEngine::restore_kv(const std::string& name) {
    int32_t status = GenieDialog_restore(m_impl->dialog_handle, name.c_str());
    if (status != GENIE_STATUS_SUCCESS)
        throw std::runtime_error("[LlmEngine] Failed to restore KV cache: " + name);
}
