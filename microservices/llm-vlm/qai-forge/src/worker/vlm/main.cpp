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
//   - EXECUTE command may include "image_refs" array ({"offset","len"} pairs
//     into the IMAGE_SHM_FD region inherited at fork() time)
//   - Images are resolved directly from shared memory and passed as
//     ImageBuffer to VlmEngine — no disk I/O
//   - RESET is a no-op (VLM context management uses SAVE_KV / RESTORE_KV)
// ─────────────────────────────────────────────────────────────────────────────

#include "vlm-engine.hpp"
#include "qai_forge/utils/Logger.h"
#include <nlohmann/json.hpp>
#include <string>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/mman.h>
#include <sstream>
#include <iomanip>
#include <random>
#include <vector>
#include <stdexcept>
#include <cstdint>

using json = nlohmann::ordered_json;

static int g_sock_fd = -1;

// Prompt shared-memory region inherited from the parent (see
// InferenceWorkerManager::startWorker() / writePromptToShm()). EXECUTE's
// "prompt_ref" {"offset","len"} points into this instead of an inline
// "prompt" string.
static uint8_t* g_prompt_shm_ptr   = nullptr;
static size_t   g_prompt_shm_bytes = 0;

// Image shared-memory region inherited from the parent (see
// InferenceWorkerManager::startWorker() / writeImagesToShm()). EXECUTE's
// "image_refs" array of {"offset","len"} points into this instead of file
// paths.
static uint8_t* g_image_shm_ptr   = nullptr;
static size_t   g_image_shm_bytes = 0;

// ─────────────────────────────────────────────────────────────────────────────
// IPC helpers
// ─────────────────────────────────────────────────────────────────────────────
static void send_message(const json& msg) {
    std::string line = msg.dump() + "\n";
    ssize_t written = ::write(g_sock_fd, line.c_str(), line.size());
    if (written < 0)
        LOG_ERROR("[vlm-worker] Socket write failed: " << strerror(errno));
}

// Bytes already read from g_sock_fd but not yet consumed as a full line —
// carried across read_message() calls so a single read() can satisfy
// multiple/partial lines without re-reading one byte at a time.
static std::string g_read_buf;

static json read_message() {
    char chunk[65536];
    while (true) {
        size_t newline_pos = g_read_buf.find('\n');
        if (newline_pos != std::string::npos) {
            std::string line = g_read_buf.substr(0, newline_pos);
            g_read_buf.erase(0, newline_pos + 1);
            return json::parse(line);
        }

        ssize_t n = ::read(g_sock_fd, chunk, sizeof(chunk));
        if (n <= 0) throw std::runtime_error("Socket closed (EOF)");
        g_read_buf.append(chunk, static_cast<size_t>(n));
    }
}

// Resolve an EXECUTE command's "prompt_ref" {"offset","len"} into the actual
// prompt text from the shared-memory region.
static std::string resolve_prompt(const json& cmd) {
    const auto& ref = cmd.at("prompt_ref");
    size_t offset = ref.value("offset", (size_t)0);
    size_t len    = ref.value("len", (size_t)0);
    if (offset + len > g_prompt_shm_bytes)
        throw std::runtime_error("prompt_ref out of bounds");
    return std::string(reinterpret_cast<const char*>(g_prompt_shm_ptr + offset), len);
}

static std::string gen_id() {
    static std::mt19937_64 rng(std::random_device{}());
    std::ostringstream oss;
    oss << std::hex << rng();
    return oss.str().substr(0, 8);
}

// ─────────────────────────────────────────────────────────────────────────────
// main — command dispatch loop
// ─────────────────────────────────────────────────────────────────────────────
int main() {
    const char* env = std::getenv("VLM_SOCKET_FD");
    if (!env) { LOG_ERROR("[vlm-worker] VLM_SOCKET_FD not set"); return 1; }
    g_sock_fd = std::stoi(env);

    const char* shm_fd_env    = std::getenv("PROMPT_SHM_FD");
    const char* shm_bytes_env = std::getenv("PROMPT_SHM_BYTES");
    if (!shm_fd_env || !shm_bytes_env) {
        LOG_ERROR("[vlm-worker] PROMPT_SHM_FD/PROMPT_SHM_BYTES not set");
        return 1;
    }
    int prompt_shm_fd = std::atoi(shm_fd_env);
    g_prompt_shm_bytes = std::strtoull(shm_bytes_env, nullptr, 10);
    void* mapped = mmap(nullptr, g_prompt_shm_bytes, PROT_READ, MAP_SHARED, prompt_shm_fd, 0);
    if (mapped == MAP_FAILED) {
        LOG_ERROR("[vlm-worker] mmap of prompt shm region failed");
        return 1;
    }
    g_prompt_shm_ptr = static_cast<uint8_t*>(mapped);

    const char* image_shm_fd_env    = std::getenv("IMAGE_SHM_FD");
    const char* image_shm_bytes_env = std::getenv("IMAGE_SHM_BYTES");
    if (!image_shm_fd_env || !image_shm_bytes_env) {
        LOG_ERROR("[vlm-worker] IMAGE_SHM_FD/IMAGE_SHM_BYTES not set");
        return 1;
    }
    int image_shm_fd = std::atoi(image_shm_fd_env);
    g_image_shm_bytes = std::strtoull(image_shm_bytes_env, nullptr, 10);
    void* image_mapped = mmap(nullptr, g_image_shm_bytes, PROT_READ, MAP_SHARED, image_shm_fd, 0);
    if (image_mapped == MAP_FAILED) {
        LOG_ERROR("[vlm-worker] mmap of image shm region failed");
        return 1;
    }
    g_image_shm_ptr = static_cast<uint8_t*>(image_mapped);

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

            GenerationConfig config;
            config.max_tokens         = cmd.value("max_tokens", 1024);
            config.temperature        = cmd.value("temperature", 1.0f);
            config.top_p              = cmd.value("top_p", 1.0f);
            config.top_k              = cmd.value("top_k", 40);
            config.presence_penalty   = cmd.value("presence_penalty", 0.0f);
            config.frequency_penalty  = cmd.value("frequency_penalty", 0.0f);
            config.bypass_think_filter = cmd.value("bypass_think_filter", false);

            // ── Resolve images from the shared-memory region ───────────────────
            // image_refs are {"offset","len"} pairs written by the parent's
            // writeImagesToShm() — resolve directly, no heap copy needed since
            // ImageBuffer::data is a const view (mirrors resolve_prompt()).
            std::vector<ImageBuffer> images;

            if (cmd.contains("image_refs") && cmd["image_refs"].is_array()) {
                for (const auto& ref : cmd["image_refs"]) {
                    size_t offset = ref.value("offset", (size_t)0);
                    size_t len    = ref.value("len", (size_t)0);
                    if (offset + len > g_image_shm_bytes) {
                        LOG_WARN("[vlm-worker] image_ref out of bounds: offset="
                                 << offset << " len=" << len);
                        continue;
                    }
                    images.push_back({g_image_shm_ptr + offset, len});
                }
            }

            // Non-streaming: accumulate all tokens before sending
            std::string accumulated;

            try {
                engine->generate(
                    resolve_prompt(cmd),
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
