// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// litert-lm-inference-worker — LiteRT-LM Inference Subprocess
//
// This binary is spawned by LiteRTLMWorkerManager (via InferenceWorkerManager
// with process_type "litert-lm"). It communicates with the parent process via
// JSON Lines over a Unix Domain Socket (socketpair).
//
// IPC Protocol:
//
//   Startup sequence:
//     1. Worker sends READY (socket connected, binary loaded)
//     2. Parent sends INIT  { model_id, config_file, sampler_config }
//     3. Worker loads .litertlm model via LiteRT-LM engine
//     4. Worker sends READY (model loaded, ready for inference)
//     5. Worker sends METADATA { model_type, max_context_length, ... }
//
//   Inference sequence (per request):
//     1. Parent sends EXECUTE { event_id, prompt, max_tokens, temperature, ... }
//     2. Worker creates conversation + calls send_message_stream
//     3. Worker sends TOKEN { event_id, content } per token
//     4. Worker sends DONE  { event_id, finish_reason }
//     5. Worker sends READY (ready for next request)
//
//   Reset / Shutdown handled via RESET / SHUTDOWN commands.
//
// Socket FD is passed via LLM_SOCKET_FD environment variable.
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/utils/Logger.h"
#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <functional>
#include <stdexcept>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <fstream>
#include <cstdint>
#include <cerrno>
#include <csignal>

// POSIX
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/mman.h>

// LiteRT-LM C API — conditionally included when SDK is available
#ifdef LITERT_LM_AVAILABLE
#include "litert_lm/c/engine.h"
#endif

using json = nlohmann::ordered_json;

// ─────────────────────────────────────────────────────────────────────────────
// IPC helpers
// ─────────────────────────────────────────────────────────────────────────────

static int g_sock_fd = -1;

// Prompt shared-memory region inherited from the parent (see
// InferenceWorkerManager::startWorker() / writePromptToShm()). EXECUTE's
// "prompt_ref" {"offset","len"} points into this instead of an inline
// "prompt" string.
static uint8_t* g_prompt_shm_ptr   = nullptr;
static size_t   g_prompt_shm_bytes = 0;

// Resolve an EXECUTE command's "prompt_ref" {"offset","len"} into the actual
// prompt text from the shared-memory region.
static std::string resolvePrompt(const json& cmd) {
    const auto& ref = cmd.at("prompt_ref");
    size_t offset = ref.value("offset", (size_t)0);
    size_t len    = ref.value("len", (size_t)0);
    if (offset + len > g_prompt_shm_bytes)
        throw std::runtime_error("prompt_ref out of bounds");
    return std::string(reinterpret_cast<const char*>(g_prompt_shm_ptr + offset), len);
}

static void sendMessage(const json& msg) {
    std::string line = msg.dump() + "\n";
    ssize_t written = ::write(g_sock_fd, line.c_str(), line.size());
    if (written < 0) {
        // Fatal — parent closed socket
        ::_exit(1);
    }
}

static json readMessage() {
    static std::string read_buf;
    char chunk[65536];
    while (true) {
        size_t newline_pos = read_buf.find('\n');
        if (newline_pos != std::string::npos) {
            std::string line = read_buf.substr(0, newline_pos);
            read_buf.erase(0, newline_pos + 1);
            return json::parse(line);
        }

        ssize_t n = ::read(g_sock_fd, chunk, sizeof(chunk));
        if (n <= 0) {
            // EOF — parent closed socket
            ::_exit(0);
        }
        read_buf.append(chunk, static_cast<size_t>(n));
    }
}

static void sendReady(const std::string& command_id = "") {
    json msg = {{"type", "READY"}};
    if (!command_id.empty()) msg["command_id"] = command_id;
    sendMessage(msg);
}

static void sendError(const std::string& event_id, const std::string& message) {
    json msg = {
        {"type", "ERROR"},
        {"event_id", event_id},
        {"message", message}
    };
    sendMessage(msg);
}

// ─────────────────────────────────────────────────────────────────────────────
// .litertlm file parser — extract jinja_prompt_template without SDK
//
// File layout:
//   [0..7]   magic "LITERTLM"
//   [8..11]  major_version (uint32 LE)
//   [12..15] minor_version (uint32 LE)
//   [16..19] patch_version (uint32 LE)
//   [20..23] padding (4 bytes)
//   [24..31] header_end_offset (uint64 LE)
//   [32..header_end) flatbuffer LiteRTLMMetaData header
//   [header_end..)  section data (TFLite models, tokenizer, LlmMetadataProto, ...)
//
// Strategy: scan section data for proto field 7 (tag=0x3a, wire type 2).
// The jinja_prompt_template is a long string (>100 bytes) starting with
// a jinja token ("{%" or "{{"). This is deterministic regardless of model.
// ─────────────────────────────────────────────────────────────────────────────

// Decode protobuf varint. Returns bytes consumed, 0 on error.
static int decodeVarint(const uint8_t* d, size_t len, uint64_t* out) {
    *out = 0;
    for (int i = 0; i < 10 && (size_t)i < len; ++i) {
        *out |= (uint64_t)(d[i] & 0x7f) << (7*i);
        if (!(d[i] & 0x80)) return i+1;
    }
    return 0;
}

// Extract jinja_prompt_template from a .litertlm model file.
// Scans section data after the flatbuffer header for proto field 7 string.
// Returns empty string if not found.
static std::string extractJinjaTemplate(const std::string& model_path) {
    std::ifstream f(model_path, std::ios::binary);
    if (!f) return "";

    // Validate magic
    char magic[8]; f.read(magic, 8);
    if (f.gcount() != 8 || std::string(magic, 8) != "LITERTLM") return "";

    // Read header_end_offset at byte 24 (skip 3x uint32 version + 4 bytes padding)
    f.ignore(16);
    uint64_t hdr_end;
    f.read(reinterpret_cast<char*>(&hdr_end), 8);
    if (!f) return "";

    // Seek to start of section data
    f.seekg(static_cast<std::streamoff>(hdr_end));
    if (!f) return "";

    // Read section data in chunks and scan for proto field 7 (tag=0x3a)
    // The LlmMetadataProto section contains a proto-encoded LlmMetadata message.
    // Field 7 = jinja_prompt_template (string, wire type 2, tag=0x3a).
    // We scan for: 0x3a + varint(len >= 100) + content starting with "{%" or "{{"
    const size_t kChunkSize = 65536;
    const size_t kMaxScan   = 64 * 1024 * 1024; // 64MB max scan
    std::vector<uint8_t> buf(kChunkSize + 256); // overlap for boundary crossing
    size_t scanned = 0;
    size_t overlap = 0;

    while (scanned < kMaxScan) {
        f.read(reinterpret_cast<char*>(buf.data() + overlap), kChunkSize);
        size_t got = (size_t)f.gcount();
        if (got == 0) break;
        size_t window = overlap + got;

        for (size_t i = 0; i + 1 < window; ++i) {
            if (buf[i] != 0x3a) continue; // field 7, wire type 2

            uint64_t sz = 0;
            int n = decodeVarint(buf.data() + i + 1, window - i - 1, &sz);
            if (!n || sz < 100 || sz > 1024*1024) continue;
            size_t str_start = i + 1 + n;
            if (str_start + 2 > window) continue;

            // Check jinja prefix
            if ((buf[str_start] == '{' && buf[str_start+1] == '%') ||
                (buf[str_start] == '{' && buf[str_start+1] == '{')) {
                // Read full string — may need to re-seek if it spans chunk boundary
                if (str_start + sz <= window) {
                    // String fully in buffer
                    std::string result(reinterpret_cast<const char*>(buf.data() + str_start), sz);
                    // Validate it's valid UTF-8 jinja (contains expected tokens)
                    if (result.find("{%") != std::string::npos || result.find("{{") != std::string::npos) {
                        return result;
                    }
                } else {
                    // String spans boundary — seek and read it directly
                    std::streamoff str_file_off =
                        static_cast<std::streamoff>(hdr_end) +
                        static_cast<std::streamoff>(scanned - overlap + i + 1 + n);
                    std::ifstream f2(model_path, std::ios::binary);
                    if (!f2) continue;
                    f2.seekg(str_file_off);
                    std::vector<uint8_t> sbuf(sz);
                    f2.read(reinterpret_cast<char*>(sbuf.data()), sz);
                    if ((size_t)f2.gcount() != sz) continue;
                    std::string result(reinterpret_cast<const char*>(sbuf.data()), sz);
                    if (result.find("{%") != std::string::npos || result.find("{{") != std::string::npos) {
                        return result;
                    }
                }
            }
        }

        scanned += got;
        // Keep last 256 bytes as overlap for boundary crossing
        overlap = std::min(window, (size_t)256);
        std::memmove(buf.data(), buf.data() + window - overlap, overlap);
        if (f.eof()) break;
    }
    return "";
}

// Extract thinking channel start/end tokens from LlmMetadata proto.
// Proto LlmMetadata field 8 = repeated Channel message:
//   Channel: field 1=channel_name(string), field 2=start(string), field 3=end(string)
// We look for a channel whose name contains "think" or "thought".
// Returns {start, end} pair, both empty if no thinking channel found.
static std::pair<std::string,std::string> extractThinkTokens(const std::string& model_path) {
    std::ifstream f(model_path, std::ios::binary);
    if (!f) return {};
    char magic[8]; f.read(magic, 8);
    if (f.gcount() != 8 || std::string(magic, 8) != "LITERTLM") return {};
    f.ignore(16);
    uint64_t hdr_end; f.read(reinterpret_cast<char*>(&hdr_end), 8); if (!f) return {};
    f.seekg(static_cast<std::streamoff>(hdr_end));

    const size_t kChunkSize = 65536;
    const size_t kMaxScan = 32 * 1024 * 1024;
    std::vector<uint8_t> buf(kChunkSize + 512);
    size_t scanned = 0, overlap = 0;

    while (scanned < kMaxScan) {
        f.read(reinterpret_cast<char*>(buf.data() + overlap), kChunkSize);
        size_t got = (size_t)f.gcount(); if (!got) break;
        size_t window = overlap + got;

        // Scan for field 8 (Channel message), tag = (8<<3)|2 = 0x42
        for (size_t i = 0; i + 1 < window; ++i) {
            if (buf[i] != 0x42) continue;
            uint64_t sz = 0; int n = decodeVarint(buf.data()+i+1, window-i-1, &sz);
            if (!n || sz < 2 || sz > 500) continue;
            size_t start = i+1+n;
            if (start+sz > window) continue;

            // Parse Channel proto: field1=name, field2=start_token, field3=end_token
            std::string ch_name, ch_start, ch_end;
            size_t pos = start;
            size_t end_pos = start + sz;
            while (pos < end_pos) {
                uint64_t tag = 0; int tn = decodeVarint(buf.data()+pos, end_pos-pos, &tag);
                if (!tn) break; pos += tn;
                int fnum = (int)(tag>>3), wt = (int)(tag&7);
                if (wt == 2) {
                    uint64_t fsz = 0; int fn = decodeVarint(buf.data()+pos, end_pos-pos, &fsz);
                    if (!fn || pos+fn+fsz > end_pos) break; pos += fn;
                    std::string val(reinterpret_cast<const char*>(buf.data()+pos), fsz); pos += fsz;
                    if (fnum == 1) ch_name = val;
                    else if (fnum == 2) ch_start = val;
                    else if (fnum == 3) ch_end = val;
                } else if (wt == 0) { uint64_t v=0; int vn=decodeVarint(buf.data()+pos,end_pos-pos,&v); if(!vn)break; pos+=vn; }
                else if (wt == 5) { if(pos+4>end_pos)break; pos+=4; }
                else if (wt == 1) { if(pos+8>end_pos)break; pos+=8; }
                else break;
            }

            // Check if this is the thinking channel
            if (!ch_name.empty() && !ch_start.empty() && !ch_end.empty()) {
                std::string lower = ch_name;
                for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
                if (lower.find("think") != std::string::npos || lower.find("thought") != std::string::npos) {
                    return {ch_start, ch_end};
                }
            }
        }

        scanned += got;
        overlap = std::min(window, (size_t)512);
        std::memmove(buf.data(), buf.data()+window-overlap, overlap);
        if (f.eof()) break;
    }
    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// LiteRT-LM Session wrapper
// ─────────────────────────────────────────────────────────────────────────────

struct LiteRTLMSession {
    std::string model_path;
    std::string jinja_template;
    std::string model_type;
    int         max_context_length = 0;
    std::string tool_call_delimiter;
    std::string tool_response_delimiter;
    std::string think_start;   // e.g. "<think>"  — from LlmMetadata.channels
    std::string think_end;     // e.g. "</think>" — empty if no thinking mode

#ifdef LITERT_LM_AVAILABLE
    LiteRtLmEngine*         engine   = nullptr;
    LiteRtLmEngineSettings* settings = nullptr;
#endif

    bool loaded = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// Persistent KV session map — keyed by session_id from EXECUTE command.
// Keeps LiteRtLmSession* alive across requests to reuse prefilled KV cache.
// Only populated when EXECUTE carries a non-empty session_id.
// ─────────────────────────────────────────────────────────────────────────────
#ifdef LITERT_LM_AVAILABLE
struct PersistentSession {
    LiteRtLmSession*    session          = nullptr;
    std::string         last_prompt;
    uint64_t            last_used_seq    = 0;  // for LRU eviction
};
// Thread safety: the worker subprocess runs a single-threaded synchronous IPC loop,
// so g_kv_sessions and g_kv_seq require no mutex. If background threads are added,
// protect all accesses with a std::mutex.
static std::unordered_map<std::string, PersistentSession> g_kv_sessions;
static uint64_t g_kv_seq = 0;

static constexpr size_t kMaxKvSessions = 16;

// Evict the least-recently-used session when over limit.
static void evictLruKvSession() {
    if (g_kv_sessions.size() <= kMaxKvSessions) return;
    auto lru = g_kv_sessions.begin();
    for (auto it = g_kv_sessions.begin(); it != g_kv_sessions.end(); ++it) {
        if (it->second.last_used_seq < lru->second.last_used_seq) {
            lru = it;
        }
    }
    LOG_INFO("[LiteRTLMWorker] KV LRU evict: session=" << lru->first);
    litert_lm_session_delete(lru->second.session);
    g_kv_sessions.erase(lru);
}
#endif
// ─────────────────────────────────────────────────────────────────────────────

static bool loadModel(LiteRTLMSession& sess, const std::string& model_path) {
    sess.model_path = model_path;

#ifdef LITERT_LM_AVAILABLE
    // Determine backend from environment (default: npu)
    const char* backend_env = std::getenv("LITERT_LM_BACKEND");
    const char* backend_str = backend_env ? backend_env : "npu";

    // Create engine settings with model path and backend
    sess.settings = litert_lm_engine_settings_create(
        model_path.c_str(), backend_str, nullptr, nullptr);
    if (!sess.settings) {
        LOG_ERROR("[LiteRTLMWorker] Failed to create engine settings for: " << model_path);
        return false;
    }

    // Configure max tokens
    const char* max_tokens_env = std::getenv("LITERT_LM_MAX_TOKENS");
    int max_tokens = max_tokens_env ? std::atoi(max_tokens_env) : 4096;
    litert_lm_engine_settings_set_max_num_tokens(sess.settings, max_tokens);

    // Set dispatch lib dir for QNN NPU support
    const char* dispatch_lib_dir = std::getenv("LITERT_DISPATCH_DIR");
    if (!dispatch_lib_dir) dispatch_lib_dir = std::getenv("LITERT_LM_DISPATCH_LIB_DIR");
    if (dispatch_lib_dir) {
        litert_lm_engine_settings_set_litert_dispatch_lib_dir(
            sess.settings, dispatch_lib_dir);
    }

    // Create engine (loads model)
    sess.engine = litert_lm_engine_create(sess.settings);
    if (!sess.engine) {
        LOG_ERROR("[LiteRTLMWorker] Failed to create engine for: " << model_path);
        litert_lm_engine_settings_delete(sess.settings);
        sess.settings = nullptr;
        return false;
    }

    // Session API: no session/conversation objects are created here. The engine
    // holds the model and tokenizer; a fresh LiteRtLmSession is created per
    // request in handleExecute() so per-request sampler/max_tokens can be applied
    // without carrying stale KV state between requests.

    // Extract jinja_prompt_template directly from the model file.
    sess.jinja_template = extractJinjaTemplate(model_path);
    if (sess.jinja_template.empty()) {
        LOG_WARN("[LiteRTLMWorker] jinja_prompt_template not found in model file: " << model_path);
    } else {
        LOG_INFO("[LiteRTLMWorker] jinja_prompt_template loaded (" << sess.jinja_template.size() << " bytes)");
    }

    // Extract thinking channel tokens (e.g. <think>/<think> for Qwen3).
    auto think_tokens = extractThinkTokens(model_path);
    sess.think_start = think_tokens.first;
    sess.think_end   = think_tokens.second;
    if (!sess.think_start.empty()) {
        LOG_INFO("[LiteRTLMWorker] thinking channel: start='" << sess.think_start
                 << "' end='" << sess.think_end << "'");
    }

    // Set metadata — read from env vars for model-specific overrides,
    // fall back to generic defaults.
    sess.model_type = std::string(
        std::getenv("LITERT_LM_MODEL_TYPE") ? std::getenv("LITERT_LM_MODEL_TYPE") : "unknown");
    sess.max_context_length = []() -> int {
        const char* env = std::getenv("LITERT_LM_DEFAULT_CONTEXT_LENGTH");
        return (env && std::atoi(env) > 0) ? std::atoi(env) : 4096;
    }();
    sess.tool_call_delimiter = std::string(
        std::getenv("LITERT_LM_TOOL_CALL_DELIMITER") ? std::getenv("LITERT_LM_TOOL_CALL_DELIMITER") : "<|tool_call|>");
    sess.tool_response_delimiter = std::string(
        std::getenv("LITERT_LM_TOOL_RESPONSE_DELIMITER") ? std::getenv("LITERT_LM_TOOL_RESPONSE_DELIMITER") : "<|tool_response|>");

#else
    // ── Stub mode (no LiteRT-LM SDK at build time) ─────────────────────────
    // Populate sensible defaults so the IPC protocol still works for testing.
    LOG_WARN("[LiteRTLMWorker] Built without LITERT_LM_AVAILABLE — running in stub mode");
    sess.jinja_template = "{% for message in messages %}{{ message.role }}: {{ message.content }}\n{% endfor %}assistant:";
    sess.model_type = "llama3";
    sess.max_context_length = []() -> int {
        const char* env = std::getenv("LITERT_LM_DEFAULT_CONTEXT_LENGTH");
        return (env && std::atoi(env) > 0) ? std::atoi(env) : 4096;
    }();
    sess.tool_call_delimiter = std::string(
        std::getenv("LITERT_LM_TOOL_CALL_DELIMITER") ? std::getenv("LITERT_LM_TOOL_CALL_DELIMITER") : "<|tool_call|>");
    sess.tool_response_delimiter = std::string(
        std::getenv("LITERT_LM_TOOL_RESPONSE_DELIMITER") ? std::getenv("LITERT_LM_TOOL_RESPONSE_DELIMITER") : "<|tool_response|>");
#endif

    sess.loaded = true;
    LOG_INFO("[LiteRTLMWorker] Model loaded: " << model_path
             << " type=" << sess.model_type
             << " ctx=" << sess.max_context_length);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// handleExecute — Run prefill + decode_async for one inference request
// ─────────────────────────────────────────────────────────────────────────────

struct StreamCallbackData {
    std::string event_id;
    bool        done = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// handleGetMetadata — Respond to __GET_METADATA__ probe from LiteRTLMWorkerManager
//
// The LiteRTLMWorkerManager sends an EXECUTE with event_id="__GET_METADATA__"
// to retrieve model metadata after startup. We respond with a single TOKEN
// containing the metadata as JSON with a "__METADATA__:" prefix, then DONE,
// then READY.
// ─────────────────────────────────────────────────────────────────────────────

static void handleGetMetadata(const LiteRTLMSession& sess, const std::string& event_id) {
    json meta = {
        {"jinja_template", sess.jinja_template},
        {"model_type", sess.model_type},
        {"max_context_length", sess.max_context_length},
        {"tool_call_delimiter", sess.tool_call_delimiter},
        {"tool_response_delimiter", sess.tool_response_delimiter},
        {"think_start", sess.think_start},
        {"think_end", sess.think_end}
    };

    // Embed metadata as a TOKEN with a special prefix that LiteRTLMWorkerManager
    // recognizes and parses out of the token stream.
    json token_msg = {
        {"type", "TOKEN"},
        {"event_id", event_id},
        {"content", "__METADATA__:" + meta.dump()},
        {"is_final", false}
    };
    sendMessage(token_msg);

    json done_msg = {
        {"type", "DONE"},
        {"event_id", event_id},
        {"finish_reason", "stop"}
    };
    sendMessage(done_msg);

    sendReady();
}

static void handleExecute(LiteRTLMSession& sess, const json& cmd) {
    std::string event_id     = cmd.value("event_id", "");
    std::string session_id   = cmd.value("session_id", "");
    bool        kv_invalidated = cmd.value("kv_invalidated", false);

    std::string prompt;
    try {
        prompt = resolvePrompt(cmd);
    } catch (const std::exception& e) {
        sendError(event_id, e.what());
        sendReady();
        return;
    }

    int max_tokens       = cmd.value("max_tokens", 512);
    float temperature    = cmd.value("temperature", 0.7f);
    float top_p          = cmd.value("top_p", 0.9f);
    int top_k            = cmd.value("top_k", 40);

    std::string text_input = prompt;
    if (cmd.contains("inputs") && cmd["inputs"].is_array()) {
        text_input.clear();
        for (const auto& input : cmd["inputs"]) {
            if (input.value("type", "text") == "text") {
                text_input = input.value("content", "");
            }
        }
    }

#ifdef LITERT_LM_AVAILABLE
    if (!sess.engine) {
        sendError(event_id, "Engine not initialized");
        sendReady();
        return;
    }

    LiteRtLmSessionConfig* config = litert_lm_session_config_create();
    if (!config) {
        sendError(event_id, "Failed to create session config");
        sendReady();
        return;
    }
    litert_lm_session_config_set_apply_prompt_template(config, false);

    LiteRtLmSamplerParams* sampler = litert_lm_sampler_params_create(kLiteRtLmSamplerTypeTopP);
    {
        const char* e_tk  = std::getenv("LITERT_LM_DEFAULT_TOP_K");
        const char* e_tp  = std::getenv("LITERT_LM_DEFAULT_TOP_P");
        const char* e_tmp = std::getenv("LITERT_LM_DEFAULT_TEMPERATURE");
        int   def_top_k   = (e_tk  && std::atoi(e_tk)  > 0)    ? std::atoi(e_tk)   : 40;
        float def_top_p   = (e_tp  && std::stof(e_tp)  > 0.0f) ? std::stof(e_tp)   : 0.9f;
        float def_temp    = (e_tmp && std::stof(e_tmp) > 0.0f) ? std::stof(e_tmp)  : 0.7f;
        litert_lm_sampler_params_set_top_k(sampler, top_k > 0 ? top_k : def_top_k);
        litert_lm_sampler_params_set_top_p(sampler, top_p > 0.0f ? top_p : def_top_p);
        litert_lm_sampler_params_set_temperature(sampler, temperature > 0.0f ? temperature : def_temp);
    }
    {
        const char* seed_env = std::getenv("LITERT_LM_SAMPLER_SEED");
        int seed = seed_env ? std::atoi(seed_env) : 0;
        litert_lm_sampler_params_set_seed(sampler, seed);
    }
    litert_lm_session_config_set_sampler_params(config, sampler);
    litert_lm_sampler_params_delete(sampler);
    litert_lm_session_config_set_max_output_tokens(config, max_tokens > 0 ? max_tokens : 512);

    // ── KV cache reuse ─────────────────────────────────────────────────────────
    LiteRtLmSession* session  = nullptr;
    std::string prefill_text  = text_input;

    if (!session_id.empty()) {
        // kv_invalidated: context eviction occurred — must rebuild session
        if (kv_invalidated) {
            auto it = g_kv_sessions.find(session_id);
            if (it != g_kv_sessions.end()) {
                litert_lm_session_delete(it->second.session);
                g_kv_sessions.erase(it);
                LOG_INFO("[LiteRTLMWorker] KV invalidated (context eviction): session=" << session_id);
            }
        } else {
            auto it = g_kv_sessions.find(session_id);
            if (it != g_kv_sessions.end() && it->second.session != nullptr) {
                const std::string& prev = it->second.last_prompt;
                if (text_input.size() > prev.size() &&
                        text_input.substr(0, prev.size()) == prev) {
                    // KV reuse: session alive and prompt is a strict prefix extension
                    session      = it->second.session;
                    prefill_text = text_input.substr(prev.size());
                    it->second.last_used_seq = ++g_kv_seq;
                    LOG_INFO("[LiteRTLMWorker] KV reuse: session=" << session_id
                             << " delta_len=" << prefill_text.size()
                             << " total_len=" << text_input.size());
                } else {
                    litert_lm_session_delete(it->second.session);
                    g_kv_sessions.erase(it);
                    LOG_INFO("[LiteRTLMWorker] KV invalidated (prompt diverged): session=" << session_id);
                }
            }
        }
    }

    if (!session) {
        // No reuse — create fresh session with config/sampler
        session = litert_lm_engine_create_session(sess.engine, config);
    }
    litert_lm_session_config_delete(config);

    if (!session) {
        sendError(event_id, "Failed to create session");
        sendReady();
        return;
    }

    // Prefill delta (or full prompt for new sessions)
    LiteRtLmInputData* input_data = litert_lm_input_data_create(
        kLiteRtLmInputDataTypeText, prefill_text.c_str(), prefill_text.size());
    if (!input_data) {
        litert_lm_session_delete(session);
        if (!session_id.empty()) g_kv_sessions.erase(session_id);
        sendError(event_id, "Failed to create input data");
        sendReady();
        return;
    }
    const LiteRtLmInputData* inputs[] = {input_data};
    int prefill_status = litert_lm_session_run_prefill(session, inputs, 1);
    litert_lm_input_data_delete(input_data);

    if (prefill_status != 0) {
        litert_lm_session_delete(session);
        if (!session_id.empty()) g_kv_sessions.erase(session_id);
        sendError(event_id, "Prefill failed: " + std::to_string(prefill_status));
        sendReady();
        return;
    }

    // Stream decode. chunk is raw text (not JSON); is_final=true sends nullptr.
    StreamCallbackData cb_data;
    cb_data.event_id = event_id;

    auto stream_callback = [](void* user_data, const char* chunk, bool is_final,
                               const char* error_msg) {
        auto* data = static_cast<StreamCallbackData*>(user_data);

        if (error_msg && error_msg[0] != '\0') {
            std::string err_str(error_msg);
            if (err_str.find("Max number of tokens") != std::string::npos) {
                json done_tok = {{"type","TOKEN"},{"event_id",data->event_id},
                                 {"content", chunk ? std::string(chunk) : ""},
                                 {"is_final",true},{"finish_reason","length"}};
                sendMessage(done_tok);
            } else {
                sendMessage(json{{"type","ERROR"},{"event_id",data->event_id},
                                 {"message",err_str}});
            }
            data->done = true;
            return;
        }

        // chunk is raw text token; nullptr when is_final=true (stream end marker)
        json token_msg = {{"type","TOKEN"},{"event_id",data->event_id},
                          {"content", chunk ? std::string(chunk) : ""},
                          {"is_final", is_final}};
        if (is_final) token_msg["finish_reason"] = "stop";
        sendMessage(token_msg);
        if (is_final) data->done = true;
    };

    int gen_status = litert_lm_session_run_decode_async(session, stream_callback, &cb_data);

    LOG_INFO("[LiteRTLMWorker] decode_async status=" << gen_status);

    if (gen_status != 0 && !cb_data.done) {
        litert_lm_session_delete(session);
        if (!session_id.empty()) g_kv_sessions.erase(session_id);
        sendError(event_id, "decode_async failed: " + std::to_string(gen_status));
        sendReady();
        return;
    }

    {
        int wait_ms = 0;
        const char* timeout_env = std::getenv("LITERT_LM_DECODE_TIMEOUT_MS");
        const int max_wait_ms = timeout_env ? std::atoi(timeout_env) : 300000; // default 5 min
        while (!cb_data.done && wait_ms < max_wait_ms) {
            ::usleep(10000);
            wait_ms += 10;
        }
        if (!cb_data.done) LOG_WARN("[LiteRTLMWorker] decode_async timed out");
    }

    json done_msg = {{"type","DONE"},{"event_id",event_id},{"finish_reason","stop"}};
    sendMessage(done_msg);

    // Persist session for KV reuse, or release immediately for stateless requests.
    if (!session_id.empty()) {
        g_kv_sessions[session_id] = {session, text_input, ++g_kv_seq};
        evictLruKvSession();  // enforce max session limit (new session has highest seq, never evicted)
        LOG_INFO("[LiteRTLMWorker] KV session stored: session=" << session_id
                 << " prompt_len=" << text_input.size()
                 << " seq=" << g_kv_seq
                 << " active_sessions=" << g_kv_sessions.size());
    } else {
        litert_lm_session_delete(session);
    }

#else
    // ── Stub mode ─────────────────────────────────────────────────────────────
    (void)temperature; (void)top_p; (void)top_k; (void)session_id; (void)kv_invalidated;
    std::string stub = "[LiteRT-LM stub] input=" + text_input.substr(0, 40);
    for (int i = 0; i < std::min(max_tokens, 20); ++i) {
        sendMessage(json{{"type","TOKEN"},{"event_id",event_id},
                         {"content", i==0 ? stub : " t"+std::to_string(i)},
                         {"is_final",false}});
    }
    sendMessage(json{{"type","DONE"},{"event_id",event_id},{"finish_reason","stop"}});
#endif

    sendReady();
}

// ─────────────────────────────────────────────────────────────────────────────
// handleReset — Reset KV cache and send READY
// ─────────────────────────────────────────────────────────────────────────────

static void handleReset(LiteRTLMSession& /*sess*/, const json& cmd) {
    std::string command_id = cmd.value("command_id", "");
#ifdef LITERT_LM_AVAILABLE
    for (auto& [sid, ps] : g_kv_sessions) {
        litert_lm_session_delete(ps.session);
    }
    if (!g_kv_sessions.empty()) {
        LOG_INFO("[LiteRTLMWorker] RESET: freed " << g_kv_sessions.size() << " KV session(s)");
        g_kv_sessions.clear();
    }
#endif
    sendReady(command_id);
}

static void handleClearSession(LiteRTLMSession& /*sess*/, const json& cmd) {
    std::string command_id = cmd.value("command_id", "");
    std::string session_id = cmd.value("session_id", "");
    if (session_id.empty()) {
        LOG_WARN("[LiteRTLMWorker] CLEAR_SESSION received with empty session_id");
        sendReady(command_id);
        return;
    }
#ifdef LITERT_LM_AVAILABLE
    auto it = g_kv_sessions.find(session_id);
    if (it != g_kv_sessions.end()) {
        litert_lm_session_delete(it->second.session);
        g_kv_sessions.erase(it);
        LOG_INFO("[LiteRTLMWorker] clearSession: freed KV for session=" << session_id);
    }
#endif
    sendReady(command_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// main — Worker entry point
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    // Ignore SIGPIPE — parent may close socket at any time
    ::signal(SIGPIPE, SIG_IGN);

    // Get socket FD from environment (set by InferenceWorkerManager::startWorker)
    const char* sock_fd_env = std::getenv("LLM_SOCKET_FD");
    if (!sock_fd_env) {
        LOG_ERROR("[LiteRTLMWorker] LLM_SOCKET_FD not set");
        return 1;
    }
    g_sock_fd = std::atoi(sock_fd_env);
    if (g_sock_fd < 0) {
        LOG_ERROR("[LiteRTLMWorker] Invalid socket FD: " << sock_fd_env);
        return 1;
    }

    // Attach the prompt shared-memory region inherited from the parent.
    const char* shm_fd_env    = std::getenv("PROMPT_SHM_FD");
    const char* shm_bytes_env = std::getenv("PROMPT_SHM_BYTES");
    if (!shm_fd_env || !shm_bytes_env) {
        LOG_ERROR("[LiteRTLMWorker] PROMPT_SHM_FD/PROMPT_SHM_BYTES not set");
        return 1;
    }
    int prompt_shm_fd = std::atoi(shm_fd_env);
    g_prompt_shm_bytes = std::strtoull(shm_bytes_env, nullptr, 10);
    void* mapped = mmap(nullptr, g_prompt_shm_bytes, PROT_READ, MAP_SHARED, prompt_shm_fd, 0);
    if (mapped == MAP_FAILED) {
        LOG_ERROR("[LiteRTLMWorker] mmap of prompt shm region failed");
        return 1;
    }
    g_prompt_shm_ptr = static_cast<uint8_t*>(mapped);

    LOG_INFO("[LiteRTLMWorker] Started, socket FD=" << g_sock_fd);

    // Step 1: Send initial READY (binary loaded, socket connected)
    sendReady();

    // Step 2: Wait for INIT command
    json init_cmd = readMessage();
    if (init_cmd.value("type", "") != "INIT") {
        LOG_ERROR("[LiteRTLMWorker] Expected INIT, got: " << init_cmd.value("type", "?"));
        return 1;
    }

    std::string model_id    = init_cmd.value("model_id", "");
    std::string config_file = init_cmd.value("config_file", "");

    LOG_INFO("[LiteRTLMWorker] INIT model_id=" << model_id
             << " config_file=" << config_file);

    // Step 3: Load model
    LiteRTLMSession sess;
    if (!loadModel(sess, config_file)) {
        sendError("", "Failed to load model: " + config_file);
        return 1;
    }

    // Step 4: Send READY (model loaded, ready for inference)
    // Must be sent BEFORE METADATA so InferenceWorkerManager::startWorker()
    // receives READY as expected and returns cleanly.
    sendReady();

    // Step 5: Send METADATA after READY (LiteRTLMWorkerManager reads it
    // after base class ensureWorkerRunning() returns).
    json metadata_msg = {
        {"type", "METADATA"},
        {"jinja_template", sess.jinja_template},
        {"model_type", sess.model_type},
        {"max_context_length", sess.max_context_length},
        {"tool_call_delimiter", sess.tool_call_delimiter},
        {"tool_response_delimiter", sess.tool_response_delimiter},
        {"think_start", sess.think_start},
        {"think_end", sess.think_end}
    };
    sendMessage(metadata_msg);

    LOG_INFO("[LiteRTLMWorker] Ready for inference: model=" << model_id);

    // ── Main request loop ──────────────────────────────────────────────────
    while (true) {
        json cmd;
        try {
            cmd = readMessage();
        } catch (const std::exception& e) {
            LOG_ERROR("[LiteRTLMWorker] Read error: " << e.what());
            break;
        }

        std::string type = cmd.value("type", "");

        if (type == "EXECUTE") {
            std::string event_id = cmd.value("event_id", "");
            if (event_id == "__GET_METADATA__") {
                handleGetMetadata(sess, event_id);
            } else {
                handleExecute(sess, cmd);
            }
        } else if (type == "RESET") {
            handleReset(sess, cmd);
        } else if (type == "CLEAR_SESSION") {
            handleClearSession(sess, cmd);
        } else if (type == "SHUTDOWN") {
            LOG_INFO("[LiteRTLMWorker] SHUTDOWN received — exiting");
#ifdef LITERT_LM_AVAILABLE
            for (auto& [sid, ps] : g_kv_sessions) {
                litert_lm_session_delete(ps.session);
            }
            g_kv_sessions.clear();
#endif
            break;
        } else if (type == "SAVE_KV") {
            // KV save/restore is not supported by the LiteRT-LM Session API.
            // Return error so callers do not silently assume state was saved.
            std::string command_id = cmd.value("command_id", "");
            std::string event_id   = cmd.value("event_id", "");
            LOG_WARN("[LiteRTLMWorker] SAVE_KV not supported by LiteRT-LM Session API");
            sendError(event_id, "SAVE_KV not supported by LiteRT-LM");
            sendReady(command_id);
        } else if (type == "RESTORE_KV") {
            std::string command_id = cmd.value("command_id", "");
            std::string event_id   = cmd.value("event_id", "");
            LOG_WARN("[LiteRTLMWorker] RESTORE_KV not supported by LiteRT-LM Session API");
            sendError(event_id, "RESTORE_KV not supported by LiteRT-LM");
            sendReady(command_id);
        } else {
            LOG_WARN("[LiteRTLMWorker] Unknown command type: " << type);
        }
    }

    // Cleanup
#ifdef LITERT_LM_AVAILABLE
    if (sess.engine) {
        litert_lm_engine_delete(sess.engine);
        sess.engine = nullptr;
    }
    if (sess.settings) {
        litert_lm_engine_settings_delete(sess.settings);
        sess.settings = nullptr;
    }
#endif

    LOG_INFO("[LiteRTLMWorker] Exiting cleanly");
    return 0;
}
