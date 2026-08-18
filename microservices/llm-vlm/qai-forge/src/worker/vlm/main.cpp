// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// genai-vlm-inference-worker — VLM Inference Worker Binary
//
// Subprocess spawned by VlmInferenceWorkerManager for VLM (vision+text) inference.
// Reads VLM_SOCKET_FD, dispatches JSON Lines commands, streams tokens back.
//
// Uses VlmEngine (Layer 4) directly — no C interface overhead.
// Non-streaming accumulation is handled here in Layer 3, not in genai-lib.
//
// Key difference from LLM worker:
//   - EXECUTE command may include "image_urls" array (local file paths)
//   - Images are loaded from disk and passed as ImageBuffer to VlmEngine
//   - RESET is a no-op (VLM context management uses SAVE_KV / RESTORE_KV)
// ─────────────────────────────────────────────────────────────────────────────

#include "vlm-engine.hpp"
#include "qai_forge/utils/Logger.h"
#include <nlohmann/json.hpp>
#include <string>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sstream>
#include <iomanip>
#include <random>
#include <fstream>
#include <vector>

using json = nlohmann::ordered_json;

static int g_sock_fd = -1;

// ─────────────────────────────────────────────────────────────────────────────
// IPC helpers
// ─────────────────────────────────────────────────────────────────────────────
static void send_message(const json& msg) {
    std::string line = msg.dump() + "\n";
    ssize_t written = ::write(g_sock_fd, line.c_str(), line.size());
    if (written < 0)
        LOG_ERROR("[vlm-worker] Socket write failed: " << strerror(errno));
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

static std::vector<uint8_t> load_image_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

// ─────────────────────────────────────────────────────────────────────────────
// main — command dispatch loop
// ─────────────────────────────────────────────────────────────────────────────
int main() {
    const char* env = std::getenv("VLM_SOCKET_FD");
    if (!env) { LOG_ERROR("[vlm-worker] VLM_SOCKET_FD not set"); return 1; }
    g_sock_fd = std::stoi(env);
    LOG_INFO("[vlm-worker] genai-vlm-inference-worker started (fd=" << g_sock_fd << ")");
    send_message({{"type", "READY"}});

    std::unique_ptr<VlmEngine> engine;

    while (true) {
        json cmd;
        try { cmd = read_message(); }
        catch (const std::exception& e) {
            LOG_ERROR("[vlm-worker] Socket read error: " << e.what());
            break;
        }

        std::string type = cmd.value("type", "");

        // ── INIT ──────────────────────────────────────────────────────────────
        if (type == "INIT") {
            engine.reset();
            std::string model_id = cmd.value("model", "");
            try {
                engine = std::make_unique<VlmEngine>(
                    model_id,
                    cmd.value("config_file", ""),
                    cmd.value("sampler_config", "sampler.json"));
                LOG_INFO("[vlm-worker] Model loaded: " << model_id);
                send_message({{"type", "READY"}});
            } catch (const std::exception& e) {
                send_message({{"type", "ERROR"},
                               {"message", "Failed to load model '" + model_id + "': " +
                                           std::string(e.what())}});
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
            std::string prompt = cmd.value("prompt", "");

            GenerationConfig config;
            config.max_tokens         = cmd.value("max_tokens", 1024);
            config.temperature        = cmd.value("temperature", 1.0f);
            config.top_p              = cmd.value("top_p", 1.0f);
            config.top_k              = cmd.value("top_k", 40);
            config.presence_penalty   = cmd.value("presence_penalty", 0.0f);
            config.frequency_penalty  = cmd.value("frequency_penalty", 0.0f);
            config.bypass_think_filter = cmd.value("bypass_think_filter", false);

            // ── Load images from local file paths ─────────────────────────────
            // image_urls are resolved to local paths by the upstream adapter
            // (preprocessVision) before the EXECUTE command is sent.
            std::vector<std::vector<uint8_t>> image_data;
            std::vector<ImageBuffer> images;

            if (cmd.contains("image_urls") && cmd["image_urls"].is_array()) {
                for (const auto& url : cmd["image_urls"]) {
                    std::string path = url.get<std::string>();
                    auto buf = load_image_file(path);
                    if (buf.empty()) {
                        LOG_WARN("[vlm-worker] Failed to load image: " << path);
                        continue;
                    }
                    image_data.push_back(std::move(buf));
                    images.push_back({image_data.back().data(), image_data.back().size()});
                }
            }

            // Non-streaming: accumulate all tokens before sending
            std::string accumulated;

            try {
                engine->generate(
                    prompt,
                    images,
                    config,
                    [&](const std::string& token, const std::string& finish_reason) {
                        if (!finish_reason.empty()) {
                            // Final token
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
                LOG_INFO("[vlm-worker] Pipeline reset");
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
                LOG_INFO("[vlm-worker] KV saved: " << name);
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
                LOG_INFO("[vlm-worker] KV restored: " << name);
                send_message({{"type", "READY"}, {"command_id", cid}});
            } catch (const std::exception& e) {
                send_message({{"type", "ERROR"},
                               {"command_id", cid},
                               {"message", std::string(e.what())}});
            }
        }

        // ── SHUTDOWN ──────────────────────────────────────────────────────────
        else if (type == "SHUTDOWN") {
            LOG_INFO("[vlm-worker] Shutdown requested");
            break;
        }

        else {
            LOG_WARN("[vlm-worker] Unknown command: " << type);
        }
    }

    engine.reset();
    ::close(g_sock_fd);
    LOG_INFO("[vlm-worker] genai-vlm-inference-worker exiting");
    return 0;
}
