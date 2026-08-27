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
    LiteRtLmEngine*             engine              = nullptr;
    LiteRtLmEngineSettings*     settings            = nullptr;
    LiteRtLmConversation*       conversation        = nullptr;
    LiteRtLmConversationConfig* conversation_config = nullptr;
    LiteRtLmSessionConfig*      session_config      = nullptr;
#endif

    bool loaded = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// loadModel — Load .litertlm model and extract metadata
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
    const char* dispatch_lib_dir = std::getenv("LITERT_LM_DISPATCH_LIB_DIR");
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

    // Create session config (used by conversation)
    sess.session_config = litert_lm_session_config_create();
    if (!sess.session_config) {
        LOG_ERROR("[LiteRTLMWorker] Failed to create session config");
        litert_lm_engine_delete(sess.engine);
        sess.engine = nullptr;
        litert_lm_engine_settings_delete(sess.settings);
        sess.settings = nullptr;
        return false;
    }
    litert_lm_session_config_set_apply_prompt_template(sess.session_config, true);

    // Create conversation config
    sess.conversation_config = litert_lm_conversation_config_create();
    if (!sess.conversation_config) {
        LOG_ERROR("[LiteRTLMWorker] Failed to create conversation config");
        litert_lm_session_config_delete(sess.session_config);
        sess.session_config = nullptr;
        litert_lm_engine_delete(sess.engine);
        sess.engine = nullptr;
        litert_lm_engine_settings_delete(sess.settings);
        sess.settings = nullptr;
        return false;
    }
    litert_lm_conversation_config_set_session_config(
        sess.conversation_config, sess.session_config);

    // Create conversation (manages KV cache across turns)
    sess.conversation = litert_lm_conversation_create(
        sess.engine, sess.conversation_config);
    if (!sess.conversation) {
        LOG_ERROR("[LiteRTLMWorker] Failed to create conversation for: " << model_path);
        litert_lm_conversation_config_delete(sess.conversation_config);
        sess.conversation_config = nullptr;
        litert_lm_session_config_delete(sess.session_config);
        sess.session_config = nullptr;
        litert_lm_engine_delete(sess.engine);
        sess.engine = nullptr;
        litert_lm_engine_settings_delete(sess.settings);
        sess.settings = nullptr;
        return false;
    }

    // Set metadata — read from env vars for model-specific overrides,
    // fall back to generic defaults.
    sess.model_type = std::string(
        std::getenv("LITERT_LM_MODEL_TYPE") ? std::getenv("LITERT_LM_MODEL_TYPE") : "unknown");
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
    // Conversation is recreated per-request, so KV cache is always clean.
    if (sess.conversation) {
        litert_lm_conversation_delete(sess.conversation);
        sess.conversation = nullptr;
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
    std::string event_id = cmd.value("event_id", "");
    std::string prompt   = cmd.value("prompt", "");
    int max_tokens       = cmd.value("max_tokens", 512);
    float temperature    = cmd.value("temperature", 0.7f);
    float top_p          = cmd.value("top_p", 0.9f);
    int top_k            = cmd.value("top_k", 40);

    // Extended: multimodal inputs array (text + base64 images)
    // If "inputs" is present, use it; otherwise fall back to "prompt"
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

    // Recreate conversation per request to apply per-request max_tokens/sampler.
    if (sess.conversation) {
        litert_lm_conversation_delete(sess.conversation);
        sess.conversation = nullptr;
    }
    if (sess.conversation_config) {
        litert_lm_conversation_config_delete(sess.conversation_config);
        sess.conversation_config = nullptr;
    }
    if (sess.session_config) {
        litert_lm_session_config_delete(sess.session_config);
        sess.session_config = nullptr;
    }

    sess.session_config = litert_lm_session_config_create();
    LiteRtLmSamplerParams* sampler_params =
        litert_lm_sampler_params_create(kLiteRtLmSamplerTypeTopP);
    litert_lm_sampler_params_set_top_k(sampler_params, top_k > 0 ? top_k : 40);
    litert_lm_sampler_params_set_top_p(sampler_params, top_p > 0.0f ? top_p : 0.9f);
    litert_lm_sampler_params_set_temperature(
        sampler_params, temperature > 0.0f ? temperature : 0.7f);
    litert_lm_sampler_params_set_seed(sampler_params, 0);
    litert_lm_session_config_set_sampler_params(sess.session_config, sampler_params);
    litert_lm_sampler_params_delete(sampler_params);
    litert_lm_session_config_set_max_output_tokens(
        sess.session_config, max_tokens > 0 ? max_tokens : 512);
    litert_lm_session_config_set_apply_prompt_template(sess.session_config, true);

    sess.conversation_config = litert_lm_conversation_config_create();
    litert_lm_conversation_config_set_session_config(
        sess.conversation_config, sess.session_config);

    sess.conversation = litert_lm_conversation_create(
        sess.engine, sess.conversation_config);
    if (!sess.conversation) {
        sendError(event_id, "Failed to create conversation");
        sendReady();
        return;
    }

    // Build user message JSON: {"role":"user","content":"<text>"}
    json user_msg = {{"role", "user"}, {"content", text_input}};
    std::string message_json = user_msg.dump();

    auto stream_callback = [](void* user_data, const char* chunk, bool is_final, const char* error_msg) {
        auto* data = static_cast<StreamCallbackData*>(user_data);

        if (error_msg && error_msg[0] != '\0') {
            std::string err_str(error_msg);
            if (err_str.find("Max number of tokens") != std::string::npos ||
                err_str.find("max_tokens") != std::string::npos) {
                json done_tok = {
                    {"type", "TOKEN"},
                    {"event_id", data->event_id},
                    {"content", chunk ? std::string(chunk) : ""},
                    {"is_final", true},
                    {"finish_reason", "length"}
                };
                sendMessage(done_tok);
                data->done = true;
                return;
            }
            json err_msg = {
                {"type", "ERROR"},
                {"event_id", data->event_id},
                {"message", err_str}
            };
            sendMessage(err_msg);
            data->done = true;
            return;
        }

        // chunk may be a JSON response — extract text content, skip thought channels
        std::string text_chunk;
        if (chunk) {
            std::string chunk_str(chunk);
            if (!chunk_str.empty() && chunk_str[0] == '{') {
                try {
                    auto j = json::parse(chunk_str);
                    // Skip thought/reasoning channels
                    if (j.contains("channels")) {
                        // thought channel — skip silently
                    } else if (j.contains("content") && j["content"].is_array()) {
                        for (const auto& part : j["content"]) {
                            if (part.value("type", "") == "text") {
                                text_chunk += part.value("text", "");
                            }
                        }
                    } else if (j.contains("content") && j["content"].is_string()) {
                        text_chunk = j.value("content", "");
                    } else if (j.contains("text")) {
                        text_chunk = j.value("text", "");
                    } else {
                        text_chunk = chunk_str;
                    }
                } catch (...) {
                    text_chunk = chunk_str;
                }
            } else {
                text_chunk = chunk_str;
            }
        }

        json token_msg = {
            {"type", "TOKEN"},
            {"event_id", data->event_id},
            {"content", text_chunk},
            {"is_final", is_final}
        };
        if (is_final) {
            token_msg["finish_reason"] = "stop";
        }
        sendMessage(token_msg);

        if (is_final) {
            data->done = true;
        }
    };

    StreamCallbackData cb_data;
    cb_data.event_id = event_id;

    int gen_status = litert_lm_conversation_send_message_stream(
        sess.conversation,
        message_json.c_str(),
        nullptr,  // extra_context
        nullptr,  // optional_args
        stream_callback, &cb_data);

    LOG_INFO("[LiteRTLMWorker] send_message_stream returned status=" << gen_status
             << " cb_done=" << cb_data.done);

    if (gen_status != 0 && !cb_data.done) {
        sendError(event_id, "Generate failed with status: " + std::to_string(gen_status));
        sendReady();
        return;
    }

    // Wait for all callbacks to complete (non-blocking call)
    {
        int wait_ms = 0;
        const int max_wait_ms = 300000; // 5 min
        while (!cb_data.done && wait_ms < max_wait_ms) {
            ::usleep(10000); // 10ms
            wait_ms += 10;
        }
        if (!cb_data.done) {
            LOG_WARN("[LiteRTLMWorker] send_message_stream timed out");
        }
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
            {"content", token},
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
        {"tool_response_delimiter", sess.tool_response_delimiter}
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
    if (sess.conversation) {
        litert_lm_conversation_delete(sess.conversation);
        sess.conversation = nullptr;
    }
    if (sess.conversation_config) {
        litert_lm_conversation_config_delete(sess.conversation_config);
        sess.conversation_config = nullptr;
    }
    if (sess.session_config) {
        litert_lm_session_config_delete(sess.session_config);
        sess.session_config = nullptr;
    }
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
