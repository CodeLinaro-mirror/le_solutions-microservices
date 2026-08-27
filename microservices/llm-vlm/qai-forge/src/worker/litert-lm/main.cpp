// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// litert-lm-inference-worker — Phase 1: LiteRT-LM Inference Subprocess
//
// This binary is spawned by LiteRTLMWorkerManager (via InferenceWorkerManager
// with process_type "litert-lm"). It communicates with the parent process via
// JSON Lines over a Unix Domain Socket (socketpair).
//
// Extended IPC Protocol (superset of the GenIE worker protocol):
//
//   Startup sequence:
//     1. Worker sends READY (socket connected, binary loaded)
//     2. Parent sends INIT  { model_id, config_file, sampler_config }
//     3. Worker loads .litertlm model, extracts metadata
//     4. Worker sends METADATA { jinja_template, model_type, max_context_length,
//                                tool_call_delimiter, tool_response_delimiter }
//     5. Worker sends READY (model loaded, ready for inference)
//
//   Inference sequence (per request):
//     1. Parent sends EXECUTE { event_id, prompt, streaming, max_tokens,
//                               temperature, top_p, top_k, inputs[] }
//     2. Worker runs prefill + decode_async
//     3. Worker sends TOKEN { event_id, token, finish_reason? } per token
//     4. Worker sends DONE  { event_id, finish_reason }
//     5. Worker sends READY (ready for next request)
//
//   Reset sequence:
//     1. Parent sends RESET { command_id }
//     2. Worker resets KV cache
//     3. Worker sends READY { command_id }
//
//   Shutdown:
//     1. Parent sends SHUTDOWN or closes socket
//     2. Worker exits cleanly
//
// The worker is intentionally dumb — no chat templating, no tool call parsing,
// no context management. All intelligence lives in LiteRTLMOrchestrator.
//
// Socket FD is passed via LLM_SOCKET_FD environment variable (same convention
// as genai-inference-worker, set by InferenceWorkerManager::startWorker).
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/utils/Logger.h"
#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <functional>
#include <stdexcept>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <csignal>

// POSIX
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>

// LiteRT-LM C API — conditionally included when SDK is available
#ifdef LITERT_LM_AVAILABLE
#include "litert_lm/c/engine.h"
#endif

using json = nlohmann::ordered_json;

// ─────────────────────────────────────────────────────────────────────────────
// IPC helpers
// ─────────────────────────────────────────────────────────────────────────────

static int g_sock_fd = -1;

static void sendMessage(const json& msg) {
    std::string line = msg.dump() + "\n";
    ssize_t written = ::write(g_sock_fd, line.c_str(), line.size());
    if (written < 0) {
        // Fatal — parent closed socket
        ::_exit(1);
    }
}

static json readMessage() {
    std::string line;
    char c;
    while (::read(g_sock_fd, &c, 1) == 1) {
        if (c == '\n') break;
        line += c;
    }
    if (line.empty()) {
        // EOF — parent closed socket
        ::_exit(0);
    }
    return json::parse(line);
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
// LiteRT-LM Session wrapper
// ─────────────────────────────────────────────────────────────────────────────

struct LiteRTLMSession {
    std::string model_path;
    std::string jinja_template;
    std::string model_type;
    int         max_context_length = 4096;
    std::string tool_call_delimiter;
    std::string tool_response_delimiter;

#ifdef LITERT_LM_AVAILABLE
    LiteRtLmSession* session = nullptr;
    LiteRtLmSettings* settings = nullptr;
#endif

    bool loaded = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// loadModel — Load .litertlm model and extract metadata
// ─────────────────────────────────────────────────────────────────────────────

static bool loadModel(LiteRTLMSession& sess, const std::string& model_path) {
    sess.model_path = model_path;

#ifdef LITERT_LM_AVAILABLE
    // Create settings
    if (litert_lm_settings_create(&sess.settings) != kLiteRtLmStatusOk) {
        LOG_ERROR("[LiteRTLMWorker] Failed to create LiteRT-LM settings");
        return false;
    }

    // Configure from environment
    const char* num_threads_env = std::getenv("LITERT_LM_NUM_THREADS");
    int num_threads = num_threads_env ? std::atoi(num_threads_env) : 4;
    litert_lm_settings_set_num_threads(sess.settings, num_threads);

    // Create session
    if (litert_lm_session_create(&sess.session, model_path.c_str(), sess.settings)
            != kLiteRtLmStatusOk) {
        LOG_ERROR("[LiteRTLMWorker] Failed to create LiteRT-LM session for: " << model_path);
        litert_lm_settings_destroy(sess.settings);
        sess.settings = nullptr;
        return false;
    }

    // Extract metadata from session
    const char* jinja_template_cstr = nullptr;
    if (litert_lm_session_get_jinja_template(sess.session, &jinja_template_cstr)
            == kLiteRtLmStatusOk && jinja_template_cstr) {
        sess.jinja_template = jinja_template_cstr;
    }

    const char* model_type_cstr = nullptr;
    if (litert_lm_session_get_model_type(sess.session, &model_type_cstr)
            == kLiteRtLmStatusOk && model_type_cstr) {
        sess.model_type = model_type_cstr;
    }

    int max_context = 0;
    if (litert_lm_session_get_max_context_length(sess.session, &max_context)
            == kLiteRtLmStatusOk) {
        sess.max_context_length = max_context;
    }

    const char* tool_call_delim = nullptr;
    if (litert_lm_session_get_tool_call_delimiter(sess.session, &tool_call_delim)
            == kLiteRtLmStatusOk && tool_call_delim) {
        sess.tool_call_delimiter = tool_call_delim;
    }

    const char* tool_resp_delim = nullptr;
    if (litert_lm_session_get_tool_response_delimiter(sess.session, &tool_resp_delim)
            == kLiteRtLmStatusOk && tool_resp_delim) {
        sess.tool_response_delimiter = tool_resp_delim;
    }

#else
    // ── Stub mode (no LiteRT-LM SDK at build time) ─────────────────────────
    // Populate sensible defaults so the IPC protocol still works for testing.
    LOG_WARN("[LiteRTLMWorker] Built without LITERT_LM_AVAILABLE — running in stub mode");
    sess.jinja_template = "{% for message in messages %}{{ message.role }}: {{ message.content }}\n{% endfor %}assistant:";
    sess.model_type = "llama3";
    sess.max_context_length = 4096;
    sess.tool_call_delimiter = "<|tool_call|>";
    sess.tool_response_delimiter = "<|tool_response|>";
#endif

    sess.loaded = true;
    LOG_INFO("[LiteRTLMWorker] Model loaded: " << model_path
             << " type=" << sess.model_type
             << " ctx=" << sess.max_context_length);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// resetKvCache — Reset the KV cache between requests
// ─────────────────────────────────────────────────────────────────────────────

static void resetKvCache(LiteRTLMSession& sess) {
#ifdef LITERT_LM_AVAILABLE
    if (sess.session) {
        litert_lm_session_reset(sess.session);
    }
#else
    (void)sess;
    LOG_DEBUG("[LiteRTLMWorker] Stub: KV cache reset");
#endif
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
        {"tool_response_delimiter", sess.tool_response_delimiter}
    };

    // Embed metadata as a TOKEN with a special prefix that LiteRTLMWorkerManager
    // recognizes and parses out of the token stream.
    json token_msg = {
        {"type", "TOKEN"},
        {"event_id", event_id},
        {"token", "__METADATA__:" + meta.dump()},
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
    std::string event_id = cmd.value("event_id", "");
    std::string prompt   = cmd.value("prompt", "");
    int max_tokens       = cmd.value("max_tokens", 512);
    float temperature    = cmd.value("temperature", 0.7f);
    float top_p          = cmd.value("top_p", 0.9f);
    int top_k            = cmd.value("top_k", 40);

    // Extended: multimodal inputs array (text + base64 images)
    // If "inputs" is present, use it; otherwise fall back to "prompt"
    std::string text_input = prompt;
    std::vector<std::string> image_inputs;

    if (cmd.contains("inputs") && cmd["inputs"].is_array()) {
        text_input.clear();
        for (const auto& input : cmd["inputs"]) {
            std::string type = input.value("type", "text");
            if (type == "text") {
                text_input = input.value("content", "");
            } else if (type == "image") {
                image_inputs.push_back(input.value("content", ""));
            }
        }
    }

    LOG_DEBUG("[LiteRTLMWorker] handleExecute event_id=" << event_id
              << " max_tokens=" << max_tokens
              << " images=" << image_inputs.size());

#ifdef LITERT_LM_AVAILABLE
    if (!sess.session) {
        sendError(event_id, "Session not initialized");
        sendReady();
        return;
    }

    // Build input data
    LiteRtLmInputData* input_data = nullptr;
    if (litert_lm_input_data_create(&input_data) != kLiteRtLmStatusOk) {
        sendError(event_id, "Failed to create input data");
        sendReady();
        return;
    }

    // Add text input
    if (!text_input.empty()) {
        litert_lm_input_data_add_text(input_data, text_input.c_str());
    }

    // Add image inputs (base64-decoded)
    for (const auto& b64_image : image_inputs) {
        // Base64 decode
        static const std::string base64_chars =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::vector<uint8_t> decoded;
        int val = 0, valb = -8;
        for (unsigned char c : b64_image) {
            if (c == '=') break;
            size_t pos = base64_chars.find(c);
            if (pos == std::string::npos) continue;
            val = (val << 6) + static_cast<int>(pos);
            valb += 6;
            if (valb >= 0) {
                decoded.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
                valb -= 8;
            }
        }
        if (!decoded.empty()) {
            litert_lm_input_data_add_image(input_data, decoded.data(), decoded.size());
        }
    }

    // Configure generation parameters
    LiteRtLmGenerationParams* gen_params = nullptr;
    litert_lm_generation_params_create(&gen_params);
    litert_lm_generation_params_set_max_tokens(gen_params, max_tokens);
    litert_lm_generation_params_set_temperature(gen_params, temperature);
    litert_lm_generation_params_set_top_p(gen_params, top_p);
    litert_lm_generation_params_set_top_k(gen_params, top_k);

    // Run prefill
    LiteRtLmOutput* prefill_output = nullptr;
    LiteRtLmStatus prefill_status =
        litert_lm_session_run_prefill(sess.session, input_data, gen_params, &prefill_output);

    litert_lm_input_data_destroy(input_data);
    litert_lm_generation_params_destroy(gen_params);

    if (prefill_status != kLiteRtLmStatusOk) {
        sendError(event_id, "Prefill failed");
        sendReady();
        return;
    }
    if (prefill_output) litert_lm_output_destroy(prefill_output);

    // Run decode async with streaming callback
    StreamCallbackData cb_data;
    cb_data.event_id = event_id;

    auto stream_callback = [](void* user_data, const LiteRtLmStreamChunk* chunk) {
        auto* data = static_cast<StreamCallbackData*>(user_data);
        if (!chunk) return;

        json token_msg = {
            {"type", "TOKEN"},
            {"event_id", data->event_id},
            {"token", chunk->text ? std::string(chunk->text) : ""},
            {"is_final", chunk->is_final}
        };
        if (chunk->is_final) {
            token_msg["finish_reason"] = "stop";
        }
        sendMessage(token_msg);

        if (chunk->is_final) {
            data->done = true;
        }
    };

    LiteRtLmStatus decode_status =
        litert_lm_session_run_decode_async(sess.session, stream_callback, &cb_data);

    if (decode_status != kLiteRtLmStatusOk) {
        sendError(event_id, "Decode failed");
        sendReady();
        return;
    }

    // Send DONE
    json done_msg = {
        {"type", "DONE"},
        {"event_id", event_id},
        {"finish_reason", "stop"}
    };
    sendMessage(done_msg);

#else
    // ── Stub mode: echo back a synthetic response ──────────────────────────
    (void)temperature; (void)top_p; (void)top_k;

    std::string stub_response = "[LiteRT-LM stub] Received: " + text_input.substr(0, 50);
    int tokens_to_emit = std::min(max_tokens, 20);

    for (int i = 0; i < tokens_to_emit; ++i) {
        std::string token = (i == 0) ? stub_response : " token" + std::to_string(i);
        json token_msg = {
            {"type", "TOKEN"},
            {"event_id", event_id},
            {"token", token},
            {"is_final", false}
        };
        sendMessage(token_msg);
    }

    json done_msg = {
        {"type", "DONE"},
        {"event_id", event_id},
        {"finish_reason", "stop"}
    };
    sendMessage(done_msg);
#endif

    // Send READY to signal completion of this request
    sendReady();
}

// ─────────────────────────────────────────────────────────────────────────────
// handleReset — Reset KV cache and send READY
// ─────────────────────────────────────────────────────────────────────────────

static void handleReset(LiteRTLMSession& sess, const json& cmd) {
    std::string command_id = cmd.value("command_id", "");
    LOG_DEBUG("[LiteRTLMWorker] RESET command_id=" << command_id);
    resetKvCache(sess);
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

    // Step 4: Send METADATA (model-specific info for orchestrator)
    json metadata_msg = {
        {"type", "METADATA"},
        {"jinja_template", sess.jinja_template},
        {"model_type", sess.model_type},
        {"max_context_length", sess.max_context_length},
        {"tool_call_delimiter", sess.tool_call_delimiter},
        {"tool_response_delimiter", sess.tool_response_delimiter}
    };
    sendMessage(metadata_msg);

    // Step 5: Send READY (model loaded, ready for inference)
    sendReady();

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
            // Special metadata probe from LiteRTLMWorkerManager
            std::string event_id = cmd.value("event_id", "");
            if (event_id == "__GET_METADATA__") {
                handleGetMetadata(sess, event_id);
            } else {
                handleExecute(sess, cmd);
            }
        } else if (type == "RESET") {
            handleReset(sess, cmd);
        } else if (type == "SHUTDOWN") {
            LOG_INFO("[LiteRTLMWorker] SHUTDOWN received — exiting");
            break;
        } else if (type == "SAVE_KV") {
            // KV save/restore not yet supported for LiteRT-LM
            std::string command_id = cmd.value("command_id", "");
            LOG_WARN("[LiteRTLMWorker] SAVE_KV not supported — sending READY anyway");
            sendReady(command_id);
        } else if (type == "RESTORE_KV") {
            std::string command_id = cmd.value("command_id", "");
            LOG_WARN("[LiteRTLMWorker] RESTORE_KV not supported — sending READY anyway");
            sendReady(command_id);
        } else {
            LOG_WARN("[LiteRTLMWorker] Unknown command type: " << type);
        }
    }

    // Cleanup
#ifdef LITERT_LM_AVAILABLE
    if (sess.session) {
        litert_lm_session_destroy(sess.session);
        sess.session = nullptr;
    }
    if (sess.settings) {
        litert_lm_settings_destroy(sess.settings);
        sess.settings = nullptr;
    }
#endif

    LOG_INFO("[LiteRTLMWorker] Exiting cleanly");
    return 0;
}
