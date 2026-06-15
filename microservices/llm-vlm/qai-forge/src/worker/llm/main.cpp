// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// genai-llm-inference-worker — LLM Inference Worker Binary
//
// Subprocess spawned by InferenceWorkerManager for LLM (text-only) inference.
// Reads LLM_SOCKET_FD, dispatches JSON Lines commands, streams tokens back.
//
// Uses LlmEngine (Layer 4) directly — no C interface overhead.
// Non-streaming accumulation is handled here in Layer 3, not in genai-lib.
// ─────────────────────────────────────────────────────────────────────────────

#include "llm-engine.hpp"
#include "qai_forge/utils/Logger.h"
#include <nlohmann/json.hpp>
#include <string>
#include <cstdlib>
#include <unistd.h>
#include <sstream>
#include <iomanip>
#include <random>

using json = nlohmann::ordered_json;

static int g_sock_fd = -1;

static void send_message(const json& msg) {
    std::string line = msg.dump() + "\n";
    ::write(g_sock_fd, line.c_str(), line.size());
}

static json read_message() {
    std::string line;
    char c;
    while (::read(g_sock_fd, &c, 1) == 1) {
        if (c == '\n') break;
        line += c;
    }
    if (line.empty()) throw std::runtime_error("Socket closed (EOF)");
    return json::parse(line);
}

static std::string gen_id() {
    static std::mt19937_64 rng(std::random_device{}());
    std::ostringstream oss;
    oss << std::hex << rng();
    return oss.str().substr(0, 8);
}

int main() {
    const char* env = std::getenv("LLM_SOCKET_FD");
    if (!env) { LOG_ERROR("[llm-worker] LLM_SOCKET_FD not set"); return 1; }
    g_sock_fd = std::stoi(env);
    LOG_INFO("[llm-worker] started (fd=" << g_sock_fd << ")");
    send_message({{"type", "READY"}});

    std::unique_ptr<LlmEngine> engine;

    while (true) {
        json cmd;
        try { cmd = read_message(); }
        catch (const std::exception& e) {
            LOG_ERROR("[llm-worker] " << e.what());
            break;
        }

        std::string type = cmd.value("type", "");

        // ── INIT ──────────────────────────────────────────────────────────────
        if (type == "INIT") {
            engine.reset();
            try {
                engine = std::make_unique<LlmEngine>(
                    cmd.value("model", ""),
                    cmd.value("config_file", ""),
                    cmd.value("sampler_config", "sampler.json"));
                send_message({{"type", "READY"}});
            } catch (const std::exception& e) {
                send_message({{"type", "ERROR"}, {"message", std::string(e.what())}});
            }
        }

        // ── EXECUTE ───────────────────────────────────────────────────────────
        else if (type == "EXECUTE") {
            if (!engine) {
                send_message({{"type", "ERROR"},
                               {"event_id", cmd.value("event_id", "")},
                               {"message", "Not initialized — send INIT first"}});
                continue;
            }

            std::string event_id = cmd.value("event_id", "");
            bool streaming = cmd.value("streaming", true);

            GenerationConfig config;
            config.max_tokens         = cmd.value("max_tokens", 1024);
            config.temperature        = cmd.value("temperature", 1.0f);
            config.top_p              = cmd.value("top_p", 1.0f);
            config.top_k              = cmd.value("top_k", 40);
            config.presence_penalty   = cmd.value("presence_penalty", 0.0f);
            config.frequency_penalty  = cmd.value("frequency_penalty", 0.0f);
            config.bypass_think_filter = cmd.value("bypass_think_filter", false);

            // Non-streaming: accumulate all tokens before sending
            std::string accumulated;

            try {
                engine->generate(
                    cmd.value("prompt", ""),
                    config,
                    [&](const std::string& token, const std::string& finish_reason) {
                        if (!finish_reason.empty()) {
                            // Final token — send accumulated or last token + DONE
                            if (streaming) {
                                if (!token.empty())
                                    send_message({{"type", "TOKEN"},
                                                  {"event_id", event_id},
                                                  {"content", token}});
                            } else {
                                accumulated += token;
                                send_message({{"type", "TOKEN"},
                                              {"event_id", event_id},
                                              {"content", accumulated}});
                            }
                            send_message({{"type", "DONE"},
                                          {"event_id", event_id},
                                          {"finish_reason", finish_reason}});
                        } else {
                            // Intermediate token
                            if (streaming) {
                                send_message({{"type", "TOKEN"},
                                              {"event_id", event_id},
                                              {"content", token}});
                            } else {
                                accumulated += token;
                            }
                        }
                    });
            } catch (const std::exception& e) {
                send_message({{"type", "ERROR"},
                               {"event_id", event_id},
                               {"message", std::string(e.what())}});
            }

            send_message({{"type", "READY"}, {"event_id", event_id}});
        }

        // ── RESET ─────────────────────────────────────────────────────────────
        else if (type == "RESET") {
            std::string cid = cmd.value("command_id", gen_id());
            try {
                if (engine) engine->reset();
                send_message({{"type", "READY"}, {"command_id", cid}});
            } catch (const std::exception& e) {
                send_message({{"type", "ERROR"},
                               {"command_id", cid},
                               {"message", std::string(e.what())}});
            }
        }

        // ── SAVE_KV ───────────────────────────────────────────────────────────
        else if (type == "SAVE_KV") {
            std::string name = cmd.value("checkpoint_name", "");
            std::string cid  = cmd.value("command_id", gen_id());
            try {
                if (engine) engine->save_kv(name);
                send_message({{"type", "READY"}, {"command_id", cid}});
            } catch (const std::exception& e) {
                send_message({{"type", "ERROR"},
                               {"command_id", cid},
                               {"message", std::string(e.what())}});
            }
        }

        // ── RESTORE_KV ────────────────────────────────────────────────────────
        else if (type == "RESTORE_KV") {
            std::string name = cmd.value("checkpoint_name", "");
            std::string cid  = cmd.value("command_id", gen_id());
            try {
                if (engine) engine->restore_kv(name);
                send_message({{"type", "READY"}, {"command_id", cid}});
            } catch (const std::exception& e) {
                send_message({{"type", "ERROR"},
                               {"command_id", cid},
                               {"message", std::string(e.what())}});
            }
        }

        // ── SHUTDOWN ──────────────────────────────────────────────────────────
        else if (type == "SHUTDOWN") {
            break;
        }

        else {
            LOG_WARN("[llm-worker] Unknown command: " << type);
        }
    }

    engine.reset();
    ::close(g_sock_fd);
    LOG_INFO("[llm-worker] exiting");
    return 0;
}
