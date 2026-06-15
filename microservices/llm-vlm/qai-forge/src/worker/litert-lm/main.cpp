// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// litert-lm-inference-worker — Layer 3 subprocess for LiteRT-LM generative AI
//
// Spawned by LiteRTLMBackend (via InferenceWorkerManager) to provide fault
// isolation. Uses the LiteRT-LM C API (c/engine.h) for LLM inference on the
// Qualcomm NPU via LiteRT dispatch.
//
// Protocol: same JSON-Lines-over-socketpair as genai-inference-worker.
// Socket FD is passed via LLM_SOCKET_FD environment variable.
//
// LiteRT-LM is built with --override_repository=litert=<our LiteRT source>
// so it uses the same LiteRT version as the rest of the stack.
//
// Key LiteRT-LM C API flow:
//   1. litert_lm_engine_settings_create(model_path, backend, NULL, NULL)
//   2. litert_lm_engine_create(settings)
//   3. litert_lm_session_config_create() + set sampler params
//   4. litert_lm_engine_create_session(engine, config)
//   5. litert_lm_session_generate_content_stream(session, inputs, n, cb, data)
//   6. Wait for is_final=true in callback, then send READY
// ─────────────────────────────────────────────────────────────────────────────

// LiteRT-LM C API — resolved via LITERT_LM_INCLUDE_PATH at build time.
// Header is staged from LiteRTLM_builder to /usr/include/litert_lm/c/engine.h
#include "litert_lm/c/engine.h"

#include <nlohmann/json.hpp>
#include <unistd.h>
#include <sys/select.h>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

using json = nlohmann::ordered_json;

// ─────────────────────────────────────────────────────────────────────────────
// IPC helpers
// ─────────────────────────────────────────────────────────────────────────────

static int g_sock_fd = -1;

static void sendMsg(const json& msg) {
    std::string line = msg.dump() + "\n";
    write(g_sock_fd, line.c_str(), line.size());
}

static json readMsg() {
    std::string line;
    char ch;
    while (true) {
        fd_set fds; FD_ZERO(&fds); FD_SET(g_sock_fd, &fds);
        struct timeval tv{300, 0};
        int r = select(g_sock_fd + 1, &fds, nullptr, nullptr, &tv);
        if (r <= 0) { std::cerr << "[litert-lm-worker] read timeout/error\n"; exit(1); }
        ssize_t n = read(g_sock_fd, &ch, 1);
        if (n <= 0) { std::cerr << "[litert-lm-worker] server disconnected\n"; exit(0); }
        if (ch == '\n') break;
        line += ch;
    }
    return json::parse(line);
}

// ─────────────────────────────────────────────────────────────────────────────
// Streaming callback state
// ─────────────────────────────────────────────────────────────────────────────

struct StreamState {
    std::string     event_id;
    int             token_count = 0;
    bool            done        = false;
    std::string     error_msg;
    std::mutex      mtx;
    std::condition_variable cv;
};

// C-compatible callback invoked from LiteRT-LM's background thread.
static void streamCallback(void* callback_data, const char* chunk,
                            bool is_final, const char* error_msg) {
    StreamState* state = static_cast<StreamState*>(callback_data);

    if (error_msg && error_msg[0] != '\0') {
        std::lock_guard<std::mutex> lock(state->mtx);
        state->error_msg = error_msg;
        state->done      = true;
        state->cv.notify_all();
        return;
    }

    if (!is_final && chunk && chunk[0] != '\0') {
        state->token_count++;
        sendMsg({
            {"type",     "TOKEN"},
            {"event_id", state->event_id},
            {"token",    chunk},
            {"index",    state->token_count}
        });
    }

    if (is_final) {
        sendMsg({
            {"type",          "DONE"},
            {"event_id",      state->event_id},
            {"token_count",   state->token_count},
            {"finish_reason", "stop"}
        });
        std::lock_guard<std::mutex> lock(state->mtx);
        state->done = true;
        state->cv.notify_all();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// LiteRT-LM engine wrapper
// ─────────────────────────────────────────────────────────────────────────────

class LiteRTLMEngine {
public:
    LiteRTLMEngine(const std::string& model_path,
                   const std::string& backend,
                   int max_tokens,
                   float temperature,
                   float top_p,
                   int top_k)
        : max_tokens_(max_tokens)
        , temperature_(temperature)
        , top_p_(top_p)
        , top_k_(top_k)
        , model_path_(model_path)
        , backend_(backend)
    {
        // Create engine settings
        settings_ = litert_lm_engine_settings_create(
            model_path.c_str(), backend.c_str(), nullptr, nullptr);
        if (!settings_)
            throw std::runtime_error("LiteRTLMEngine: failed to create engine settings");

        litert_lm_engine_settings_set_max_num_tokens(settings_, max_tokens * 4);

        // Create engine
        engine_ = litert_lm_engine_create(settings_);
        if (!engine_)
            throw std::runtime_error("LiteRTLMEngine: failed to create engine for: " + model_path);

        // Create initial session
        createSession(max_tokens, temperature, top_p, top_k);
    }

    ~LiteRTLMEngine() {
        destroySession();
        if (engine_)   { litert_lm_engine_delete(engine_);   engine_   = nullptr; }
        if (settings_) { litert_lm_engine_settings_delete(settings_); settings_ = nullptr; }
    }

    void generate(const std::string& event_id,
                  int max_tokens,
                  float temperature,
                  float top_p,
                  int top_k,
                  const std::string& prompt)
    {
        // Recreate session if sampling params changed
        if (max_tokens != max_tokens_ || temperature != temperature_ ||
            top_p != top_p_ || top_k != top_k_) {
            destroySession();
            createSession(max_tokens, temperature, top_p, top_k);
        }

        // Build input
        LiteRtLmInputData input;
        input.type = kLiteRtLmInputDataTypeText;
        input.data = prompt.c_str();
        input.size = prompt.size();

        // Set up streaming state
        StreamState state;
        state.event_id = event_id;

        // Start streaming (non-blocking — callback runs on background thread)
        int rc = litert_lm_session_generate_content_stream(
            session_, &input, 1, streamCallback, &state);
        if (rc != 0)
            throw std::runtime_error("LiteRTLMEngine: generate_content_stream failed");

        // Wait for completion
        {
            std::unique_lock<std::mutex> lock(state.mtx);
            state.cv.wait(lock, [&state] { return state.done; });
        }

        if (!state.error_msg.empty())
            throw std::runtime_error("LiteRTLMEngine: stream error: " + state.error_msg);

        // Signal stream complete
        sendMsg({{"type", "READY"}});
    }

    void reset() {
        // Reset by recreating the session
        destroySession();
        createSession(max_tokens_, temperature_, top_p_, top_k_);
    }

private:
    void createSession(int max_tokens, float temperature, float top_p, int top_k) {
        max_tokens_  = max_tokens;
        temperature_ = temperature;
        top_p_       = top_p;
        top_k_       = top_k;

        LiteRtLmSessionConfig* config = litert_lm_session_config_create();
        if (!config)
            throw std::runtime_error("LiteRTLMEngine: failed to create session config");

        litert_lm_session_config_set_max_output_tokens(config, max_tokens);

        LiteRtLmSamplerParams sampler{};
        sampler.type        = kLiteRtLmSamplerTypeTopP;
        sampler.top_k       = top_k;
        sampler.top_p       = top_p;
        sampler.temperature = temperature;
        sampler.seed        = 0;
        litert_lm_session_config_set_sampler_params(config, &sampler);

        session_ = litert_lm_engine_create_session(engine_, config);
        litert_lm_session_config_delete(config);

        if (!session_)
            throw std::runtime_error("LiteRTLMEngine: failed to create session");
    }

    void destroySession() {
        if (session_) {
            litert_lm_session_delete(session_);
            session_ = nullptr;
        }
    }

    LiteRtLmEngineSettings* settings_ = nullptr;
    LiteRtLmEngine*         engine_   = nullptr;
    LiteRtLmSession*        session_  = nullptr;

    int         max_tokens_;
    float       temperature_;
    float       top_p_;
    int         top_k_;
    std::string model_path_;
    std::string backend_;
};

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    const char* fd_env = std::getenv("LLM_SOCKET_FD");
    if (!fd_env) { std::cerr << "[litert-lm-worker] LLM_SOCKET_FD not set\n"; return 1; }
    g_sock_fd = std::atoi(fd_env);

    // Suppress verbose LiteRT-LM logs (0=VERBOSE … 4=ERROR)
    litert_lm_set_min_log_level(3);

    sendMsg({{"type", "READY"}});

    std::unique_ptr<LiteRTLMEngine> engine;

    while (true) {
        json msg = readMsg();
        std::string type = msg.value("type", "");

        if (type == "SHUTDOWN") break;

        if (type == "INIT") {
            std::string model_id    = msg.value("model_id",    "");
            std::string config_file = msg.value("config_file", "");
            // Backend: "npu" uses Qualcomm NPU via LiteRT dispatch.
            // Override with LITERT_LM_BACKEND env var if needed (e.g. "cpu" for testing).
            const char* backend_env = std::getenv("LITERT_LM_BACKEND");
            std::string backend     = backend_env ? backend_env : "npu";

            try {
                engine = std::make_unique<LiteRTLMEngine>(
                    config_file.empty() ? model_id : config_file,
                    backend,
                    /*max_tokens=*/512,
                    /*temperature=*/1.0f,
                    /*top_p=*/1.0f,
                    /*top_k=*/40);
                sendMsg({{"type", "READY"}});
            } catch (const std::exception& e) {
                sendMsg({{"type", "ERROR"}, {"message", e.what()}});
            }
            continue;
        }

        if (type == "EXECUTE") {
            std::string event_id = msg.value("event_id", "");
            if (!engine) {
                sendMsg({{"type","ERROR"},{"event_id",event_id},{"message","Engine not initialized"}});
                sendMsg({{"type","READY"}});
                continue;
            }
            try {
                engine->generate(
                    event_id,
                    msg.value("max_new_tokens",   512),
                    msg.value("temperature",      1.0f),
                    msg.value("top_p",            1.0f),
                    msg.value("top_k",            40),
                    msg.value("prompt",           ""));
            } catch (const std::exception& e) {
                sendMsg({{"type","ERROR"},{"event_id",event_id},{"message",e.what()}});
                sendMsg({{"type","READY"}});
            }
            continue;
        }

        if (type == "RESET") {
            std::string command_id = msg.value("command_id", "");
            if (engine) { try { engine->reset(); } catch (...) {} }
            sendMsg({{"type","READY"},{"command_id",command_id}});
            continue;
        }

        // SAVE_KV / RESTORE_KV — LiteRT-LM session doesn't expose KV save/restore
        // in the C API; acknowledge without action.
        if (type == "SAVE_KV" || type == "RESTORE_KV") {
            std::string command_id = msg.value("command_id", "");
            sendMsg({{"type","READY"},{"command_id",command_id}});
            continue;
        }

        std::cerr << "[litert-lm-worker] unknown command: " << type << "\n";
    }

    return 0;
}
