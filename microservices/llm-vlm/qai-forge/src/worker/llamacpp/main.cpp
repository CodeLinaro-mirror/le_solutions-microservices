// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// llamacpp-inference-worker — llama.cpp Inference Subprocess
//
// Spawned by LlamaCppWorkerManager (via InferenceWorkerManager with
// process_type "llamacpp"). Talks directly to libllama — no llama-server
// subprocess, no HTTP/SSE. IPC is JSON Lines over a Unix Domain Socket
// (socketpair), mirroring litert-lm-inference-worker's protocol exactly.
//
//   Startup sequence:
//     1. Worker sends READY (socket connected, binary loaded)
//     2. Parent sends INIT { model, config_file }
//        config_file is a small JSON sidecar {model_path, context_size,
//        n_gpu_layers} written by LlamaCppBackend::loadModel() — not the
//        raw .gguf path.
//     3. Worker loads the GGUF model + context via libllama
//     4. Worker sends READY (model loaded, ready for inference)
//
//   Inference sequence (per request), via createStructuredExecuteCommand():
//     1. Parent sends EXECUTE { event_id, messages, tools, streaming,
//        max_tokens, temperature, top_p, top_k, presence_penalty,
//        frequency_penalty, image_refs? }
//     2. Worker clears the KV cache, renders messages/tools through the
//        model's own embedded chat template (common_chat_templates_apply),
//        tokenizes, and decodes token-by-token. If image_refs are present
//        and an mmproj projector was loaded (QAI_FORGE_LLAMACPP_MTMD build),
//        a media marker is injected into the last message before rendering
//        and the prefill goes through libmtmd instead of plain llama_tokenize.
//     3. Worker sends TOKEN { event_id, content } per emitted piece
//     4. Worker sends DONE { event_id, finish_reason }
//     5. Worker sends READY (ready for next request)
//
//   RESET clears the KV cache. SAVE_KV/RESTORE_KV are not supported in the
//   MVP — acked with READY anyway, matching litert-lm-inference-worker's stub.
//
// Socket FD is passed via LLM_SOCKET_FD. PROMPT_SHM_FD/PROMPT_SHM_BYTES are
// mapped for parity with InferenceWorkerManager's contract but go unused
// here: the structured EXECUTE command carries messages/tools inline, not
// via the shm-backed prompt_ref path used by the plain-text EXECUTE variant.
// IMAGE_SHM_FD/IMAGE_SHM_BYTES map the image shared-memory region that
// EXECUTE's "image_refs" ({"offset","len"} pairs) resolve into — mirrors
// vlm-worker's identical resolution pattern.
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/utils/Logger.h"
#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <stdexcept>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <fstream>
#include <thread>

// POSIX
#include <unistd.h>
#include <sys/mman.h>

// llama.cpp direct API
#include "llama.h"
#include "ggml.h"
#include "ggml-backend.h"
#include "common/common.h"
#include "common/sampling.h"
#include "common/chat.h"
#include "common/log.h"

// libmtmd: vision/--mmproj support. Only linked in when the build found
// libmtmd (see qai-forge/CMakeLists.txt) — absent, this worker is text-only.
#ifdef QAI_FORGE_LLAMACPP_MTMD
#include "mtmd.h"
#include "mtmd-helper.h"
#endif

using json = nlohmann::ordered_json;

// ─────────────────────────────────────────────────────────────────────────────
// IPC helpers — identical shape to litert-lm-inference-worker/main.cpp
// ─────────────────────────────────────────────────────────────────────────────

static int g_sock_fd = -1;

static uint8_t* g_prompt_shm_ptr   = nullptr;
static size_t   g_prompt_shm_bytes = 0;

static uint8_t* g_image_shm_ptr   = nullptr;
static size_t   g_image_shm_bytes = 0;

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
// LlamaCppSession — owns the loaded model/context for this worker process
// ─────────────────────────────────────────────────────────────────────────────

struct LlamaCppSession {
    std::string model_path;
    std::string mmproj_path;
    int         context_size = 4096;
    int         n_gpu_layers = 0;

    llama_model*        model = nullptr;
    llama_context*      ctx   = nullptr;
    const llama_vocab*  vocab = nullptr;
    common_chat_templates_ptr chat_templates;

#ifdef QAI_FORGE_LLAMACPP_MTMD
    mtmd_context* mtmd_ctx = nullptr;
#endif

    bool loaded = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// loadModel — parse the JSON sidecar config_file and load the GGUF model
// ─────────────────────────────────────────────────────────────────────────────

static bool loadModel(LlamaCppSession& sess, const std::string& config_file) {
    std::ifstream in(config_file);
    if (!in) {
        LOG_ERROR("[LlamaCppWorker] Failed to open config_file: " << config_file);
        return false;
    }
    json cfg;
    try {
        in >> cfg;
    } catch (const std::exception& e) {
        LOG_ERROR("[LlamaCppWorker] Failed to parse config_file: " << e.what());
        return false;
    }

    sess.model_path   = cfg.value("model_path", "");
    sess.mmproj_path  = cfg.value("mmproj_path", "");
    sess.context_size = cfg.value("context_size", 4096);
    sess.n_gpu_layers  = cfg.value("n_gpu_layers", 0);

    if (sess.model_path.empty()) {
        LOG_ERROR("[LlamaCppWorker] config_file missing model_path: " << config_file);
        return false;
    }

    ggml_backend_load_all();

    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = sess.n_gpu_layers;

    sess.model = llama_model_load_from_file(sess.model_path.c_str(), model_params);
    if (!sess.model) {
        LOG_ERROR("[LlamaCppWorker] Failed to load model: " << sess.model_path);
        return false;
    }

    sess.vocab = llama_model_get_vocab(sess.model);

    // GGML_DEFAULT_N_THREADS (4) leaves cores idle on multi-core boards —
    // use all detected cores for CPU-side work (sampling, any op the NPU
    // backend doesn't cover, and the CLIP encoder's own CPU fallback).
    const unsigned int hw_threads = std::thread::hardware_concurrency();
    const int n_threads = hw_threads > 0 ? static_cast<int>(hw_threads) : 4;

    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx   = sess.context_size;
    ctx_params.n_batch = sess.context_size;
    ctx_params.n_threads       = n_threads;
    ctx_params.n_threads_batch = n_threads;

    sess.ctx = llama_init_from_model(sess.model, ctx_params);
    if (!sess.ctx) {
        LOG_ERROR("[LlamaCppWorker] Failed to create context for: " << sess.model_path);
        llama_model_free(sess.model);
        sess.model = nullptr;
        return false;
    }

    // Empty override => reads the model's own embedded jinja template
    // (llama_model_chat_template()) rather than a server-supplied one.
    sess.chat_templates = common_chat_templates_init(sess.model, "");
    if (!sess.chat_templates) {
        LOG_ERROR("[LlamaCppWorker] Failed to init chat templates for: " << sess.model_path);
        llama_free(sess.ctx);
        sess.ctx = nullptr;
        llama_model_free(sess.model);
        sess.model = nullptr;
        return false;
    }

    sess.loaded = true;
    LOG_INFO("[LlamaCppWorker] Model loaded: " << sess.model_path
             << " ctx=" << sess.context_size << " ngl=" << sess.n_gpu_layers);

#ifdef QAI_FORGE_LLAMACPP_MTMD
    // Load the paired mmproj projector, if any. A missing/failed projector
    // is not fatal to model load — the worker just stays text-only, since
    // the same GGUF can be used with or without an image in a given request.
    if (!sess.mmproj_path.empty()) {
        mtmd_context_params mtmd_params = mtmd_context_params_default();
        // ggml-hexagon doesn't correctly support/gate flash-attention in the
        // CLIP graph (llama.cpp PR #16837 made CLIP default to FA) — it gets
        // dispatched to the DSP instead of falling back to CPU and faults
        // ("dspqueue_read failed"). Keep the vision encoder on CPU; the main
        // LLM decode still runs on NPU via n_gpu_layers.
        mtmd_params.use_gpu = false;
        mtmd_params.n_threads = n_threads;
        sess.mtmd_ctx = mtmd_init_from_file(sess.mmproj_path.c_str(), sess.model, mtmd_params);
        if (!sess.mtmd_ctx) {
            LOG_WARN("[LlamaCppWorker] Failed to init mtmd context from mmproj: "
                     << sess.mmproj_path << " — continuing text-only");
        } else {
            LOG_INFO("[LlamaCppWorker] mmproj loaded: " << sess.mmproj_path);
        }
    }
#endif

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// resetKvCache — FULL_RECOMPUTE: every EXECUTE/RESET starts from a clean
// context (see the migration plan's scope decision — no incremental KV
// reuse across turns in the MVP).
// ─────────────────────────────────────────────────────────────────────────────

static void resetKvCache(LlamaCppSession& sess) {
    if (sess.ctx) {
        llama_memory_clear(llama_get_memory(sess.ctx), /*data=*/true);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// handleExecute — render the prompt via the chat template, decode, sample,
// and stream tokens back over the socket.
// ─────────────────────────────────────────────────────────────────────────────

static void handleExecute(LlamaCppSession& sess, const json& cmd) {
    std::string event_id = cmd.value("event_id", "");

    if (!sess.loaded || !sess.ctx) {
        sendError(event_id, "Model not loaded");
        sendReady();
        return;
    }

    int   max_tokens        = cmd.value("max_tokens", 512);
    float temperature       = cmd.value("temperature", 0.7f);
    float top_p             = cmd.value("top_p", 0.9f);
    int   top_k             = cmd.value("top_k", 40);
    float presence_penalty  = cmd.value("presence_penalty", 0.0f);
    float frequency_penalty = cmd.value("frequency_penalty", 0.0f);

    resetKvCache(sess);

    // ── Resolve images from the shared-memory region ────────────────────────
    // image_refs are {"offset","len"} pairs written by the parent's
    // writeImagesToShm() — mirrors vlm-worker's identical resolution pattern.
    std::vector<std::pair<const uint8_t*, size_t>> image_views;
    if (cmd.contains("image_refs") && cmd["image_refs"].is_array()) {
        for (const auto& ref : cmd["image_refs"]) {
            size_t offset = ref.value("offset", (size_t)0);
            size_t len    = ref.value("len", (size_t)0);
            if (offset + len > g_image_shm_bytes) {
                LOG_WARN("[LlamaCppWorker] image_ref out of bounds: offset="
                         << offset << " len=" << len);
                continue;
            }
            image_views.emplace_back(g_image_shm_ptr + offset, len);
        }
    }

#ifdef QAI_FORGE_LLAMACPP_MTMD
    const bool use_mtmd = !image_views.empty() && sess.mtmd_ctx != nullptr;
#else
    const bool use_mtmd = false;
#endif
    if (!image_views.empty() && !use_mtmd) {
        LOG_WARN("[LlamaCppWorker] Received " << image_views.size()
                 << " image(s) but no mmproj is loaded for this model — "
                    "ignoring images, falling back to text-only");
    }

    // ── Render messages/tools via the model's own chat template ───────────
    json messages_json = cmd.value("messages", json::array());
#ifdef QAI_FORGE_LLAMACPP_MTMD
    if (use_mtmd && !messages_json.empty()) {
        // Inject one media marker per image into the last message's content
        // so the rendered prompt contains mtmd_default_marker() at the exact
        // spot mtmd_tokenize() expects to substitute each image chunk.
        // Prepended, not appended — most vision models require the marker
        // before the accompanying text (llama.cpp PR #17616).
        std::string markers;
        for (size_t i = 0; i < image_views.size(); ++i) {
            markers += mtmd_default_marker();
        }
        json& last = messages_json.back();
        json& content = last["content"];
        if (content.is_string()) {
            content = markers + content.get<std::string>();
        } else if (content.is_array()) {
            content.insert(content.begin(), {{"type", "text"}, {"text", markers}});
        } else {
            content = markers;
        }
    }
#endif

    std::string prompt;
    try {
        common_chat_templates_inputs inputs;
        inputs.use_jinja = true;
        inputs.add_generation_prompt = true;
        inputs.messages = common_chat_msgs_parse_oaicompat(messages_json);

        json tools = cmd.value("tools", json::array());
        if (tools.is_array() && !tools.empty()) {
            inputs.tools = common_chat_tools_parse_oaicompat(tools);
        }

        common_chat_params chat_params = common_chat_templates_apply(sess.chat_templates.get(), inputs);
        prompt = chat_params.prompt;
    } catch (const std::exception& e) {
        sendError(event_id, std::string("Failed to render chat template: ") + e.what());
        sendReady();
        return;
    }

    int n_ctx = (int)llama_n_ctx(sess.ctx);

    // ── Prefill: plain tokenize+decode, or mtmd tokenize+eval for images ───
    // `have_pending_batch` tracks whether a batch still needs llama_decode()
    // before the next sample — false right after an mtmd prefill, since
    // mtmd_helper_eval_chunks() already ran decode internally (logits_last).
    std::vector<llama_token> prompt_tokens;
    llama_batch pending_batch{};
    bool have_pending_batch = false;

#ifdef QAI_FORGE_LLAMACPP_MTMD
    if (use_mtmd) {
        mtmd::bitmaps mtmd_bitmaps;
        mtmd::input_chunks mtmd_chunks(mtmd_input_chunks_init());
        for (const auto& view : image_views) {
            auto wrapper = mtmd_helper_bitmap_init_from_buf(
                sess.mtmd_ctx, view.first, view.second, /*placeholder=*/false);
            if (!wrapper.bitmap) {
                sendError(event_id, "Failed to decode image bitmap");
                sendReady();
                return;
            }
            mtmd_bitmaps.entries.emplace_back(wrapper.bitmap);
        }

        mtmd_input_text input_text;
        input_text.text = prompt.c_str();
        input_text.text_len = prompt.size();
        input_text.add_special = true;
        input_text.parse_special = true;

        auto bitmap_ptrs = mtmd_bitmaps.c_ptr();
        int32_t tok_res = mtmd_tokenize(sess.mtmd_ctx, mtmd_chunks.ptr.get(),
                                        &input_text, bitmap_ptrs.data(), bitmap_ptrs.size());
        if (tok_res != 0) {
            sendError(event_id, "mtmd_tokenize failed (code " + std::to_string(tok_res) + ")");
            sendReady();
            return;
        }

        llama_pos new_n_past = 0;
        int32_t eval_res = mtmd_helper_eval_chunks(
            sess.mtmd_ctx, sess.ctx, mtmd_chunks.ptr.get(),
            /*n_past=*/0, /*seq_id=*/0, /*n_batch=*/(int32_t)llama_n_batch(sess.ctx),
            /*logits_last=*/true, &new_n_past);
        if (eval_res != 0) {
            sendError(event_id, "mtmd_helper_eval_chunks failed (code " + std::to_string(eval_res) + ")");
            sendReady();
            return;
        }
        // have_pending_batch stays false: logits for the first sample below
        // are already available from the eval above.
    } else
#endif
    {
        // ── Tokenize the rendered prompt ────────────────────────────────────
        int n_prompt_tokens = -llama_tokenize(sess.vocab, prompt.c_str(), (int32_t)prompt.size(),
                                               nullptr, 0, /*add_special=*/true, /*parse_special=*/true);
        if (n_prompt_tokens <= 0) {
            sendError(event_id, "Failed to count prompt tokens");
            sendReady();
            return;
        }
        prompt_tokens.resize(n_prompt_tokens);
        if (llama_tokenize(sess.vocab, prompt.c_str(), (int32_t)prompt.size(),
                            prompt_tokens.data(), (int32_t)prompt_tokens.size(),
                            /*add_special=*/true, /*parse_special=*/true) < 0) {
            sendError(event_id, "Failed to tokenize prompt");
            sendReady();
            return;
        }

        if ((int)prompt_tokens.size() >= n_ctx) {
            sendError(event_id, "Prompt exceeds context size");
            sendReady();
            return;
        }

        pending_batch = llama_batch_get_one(prompt_tokens.data(), (int32_t)prompt_tokens.size());
        have_pending_batch = true;
    }

    // ── Build the sampler chain for this request ───────────────────────────
    common_params_sampling sparams;
    sparams.top_k           = top_k > 0 ? top_k : 40;
    sparams.top_p           = top_p > 0.0f ? top_p : 1.0f;
    sparams.temp            = temperature;
    sparams.penalty_freq    = frequency_penalty;
    sparams.penalty_present = presence_penalty;

    common_sampler* sampler = common_sampler_init(sess.model, sparams);
    if (!sampler) {
        sendError(event_id, "Failed to initialize sampler");
        sendReady();
        return;
    }

    std::string finish_reason = "stop";
    int n_generated = 0;

    while (true) {
        int n_ctx_used = (int)llama_memory_seq_pos_max(llama_get_memory(sess.ctx), 0) + 1;
        if (have_pending_batch) {
            if (n_ctx_used + pending_batch.n_tokens > n_ctx) {
                finish_reason = "length";
                break;
            }

            if (llama_decode(sess.ctx, pending_batch) != 0) {
                sendError(event_id, "llama_decode failed");
                common_sampler_free(sampler);
                sendReady();
                return;
            }
        } else if (n_ctx_used >= n_ctx) {
            finish_reason = "length";
            break;
        }

        llama_token new_token = common_sampler_sample(sampler, sess.ctx, -1);
        common_sampler_accept(sampler, new_token, /*is_generated=*/true);

        if (llama_vocab_is_eog(sess.vocab, new_token)) {
            finish_reason = "stop";
            break;
        }

        char piece_buf[256];
        int piece_len = llama_token_to_piece(sess.vocab, new_token, piece_buf, sizeof(piece_buf), 0, true);
        if (piece_len < 0) {
            sendError(event_id, "Failed to detokenize output");
            common_sampler_free(sampler);
            sendReady();
            return;
        }

        json token_msg = {
            {"type", "TOKEN"},
            {"event_id", event_id},
            {"content", std::string(piece_buf, piece_len)}
        };
        sendMessage(token_msg);

        ++n_generated;
        if (n_generated >= max_tokens) {
            finish_reason = "length";
            break;
        }

        pending_batch = llama_batch_get_one(&new_token, 1);
        have_pending_batch = true;
    }

    common_sampler_free(sampler);

    json done_msg = {
        {"type", "DONE"},
        {"event_id", event_id},
        {"finish_reason", finish_reason}
    };
    sendMessage(done_msg);

    // Send READY to signal completion of this request
    sendReady();
}

// ─────────────────────────────────────────────────────────────────────────────
// handleReset — clear the KV cache and send READY
// ─────────────────────────────────────────────────────────────────────────────

static void handleReset(LlamaCppSession& sess, const json& cmd) {
    std::string command_id = cmd.value("command_id", "");
    LOG_DEBUG("[LlamaCppWorker] RESET command_id=" << command_id);
    resetKvCache(sess);
    sendReady(command_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// main — Worker entry point
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    // Ignore SIGPIPE — parent may close socket at any time
    ::signal(SIGPIPE, SIG_IGN);

    // llama.cpp's own logging (LLAMA_LOG_*/LOG_DBG macros) is independent of
    // QAIServe's LOG_LEVEL — it defaults to a threshold that hides the
    // per-backend buffer-size lines (load_tensors:, llama_kv_cache:,
    // sched_reserve:) equivalent to llama-server's `-lv 4`. This worker has
    // no CLI, so exposing that flag would otherwise require a rebuild.
    common_log_set_verbosity_thold(4);
    if (const char* verbosity_env = std::getenv("LLAMACPP_LOG_VERBOSITY")) {
        common_log_set_verbosity_thold(std::atoi(verbosity_env));
    }

    // Get socket FD from environment (set by InferenceWorkerManager::startWorker)
    const char* sock_fd_env = std::getenv("LLM_SOCKET_FD");
    if (!sock_fd_env) {
        LOG_ERROR("[LlamaCppWorker] LLM_SOCKET_FD not set");
        return 1;
    }
    g_sock_fd = std::atoi(sock_fd_env);
    if (g_sock_fd < 0) {
        LOG_ERROR("[LlamaCppWorker] Invalid socket FD: " << sock_fd_env);
        return 1;
    }

    // Attach the prompt shared-memory region inherited from the parent.
    // Unused by the structured EXECUTE path but always provided by the
    // base InferenceWorkerManager, so map it for protocol parity.
    const char* shm_fd_env    = std::getenv("PROMPT_SHM_FD");
    const char* shm_bytes_env = std::getenv("PROMPT_SHM_BYTES");
    if (shm_fd_env && shm_bytes_env) {
        int prompt_shm_fd = std::atoi(shm_fd_env);
        g_prompt_shm_bytes = std::strtoull(shm_bytes_env, nullptr, 10);
        void* mapped = mmap(nullptr, g_prompt_shm_bytes, PROT_READ, MAP_SHARED, prompt_shm_fd, 0);
        if (mapped != MAP_FAILED) {
            g_prompt_shm_ptr = static_cast<uint8_t*>(mapped);
        } else {
            LOG_WARN("[LlamaCppWorker] mmap of prompt shm region failed — continuing without it");
        }
    }

    // Attach the image shared-memory region inherited from the parent —
    // InferenceWorkerManager now creates this for process_type "llamacpp"
    // too (see Section H), used to resolve EXECUTE's "image_refs" for
    // vision requests against a model with an mmproj projector loaded.
    const char* image_shm_fd_env    = std::getenv("IMAGE_SHM_FD");
    const char* image_shm_bytes_env = std::getenv("IMAGE_SHM_BYTES");
    if (image_shm_fd_env && image_shm_bytes_env) {
        int image_shm_fd = std::atoi(image_shm_fd_env);
        g_image_shm_bytes = std::strtoull(image_shm_bytes_env, nullptr, 10);
        void* mapped = mmap(nullptr, g_image_shm_bytes, PROT_READ, MAP_SHARED, image_shm_fd, 0);
        if (mapped != MAP_FAILED) {
            g_image_shm_ptr = static_cast<uint8_t*>(mapped);
        } else {
            LOG_WARN("[LlamaCppWorker] mmap of image shm region failed — continuing without vision support");
        }
    }

    LOG_INFO("[LlamaCppWorker] Started, socket FD=" << g_sock_fd);

    // Step 1: Send initial READY (binary loaded, socket connected)
    sendReady();

    // Step 2: Wait for INIT command
    json init_cmd = readMessage();
    if (init_cmd.value("type", "") != "INIT") {
        LOG_ERROR("[LlamaCppWorker] Expected INIT, got: " << init_cmd.value("type", "?"));
        return 1;
    }

    std::string model_id    = init_cmd.value("model", "");
    std::string config_file = init_cmd.value("config_file", "");

    LOG_INFO("[LlamaCppWorker] INIT model=" << model_id << " config_file=" << config_file);

    // Step 3: Load model
    LlamaCppSession sess;
    if (!loadModel(sess, config_file)) {
        sendError("", "Failed to load model from config_file: " + config_file);
        return 1;
    }

    // Step 4: Send READY (model loaded, ready for inference)
    sendReady();

    LOG_INFO("[LlamaCppWorker] Ready for inference: model=" << model_id);

    // ── Main request loop ──────────────────────────────────────────────────
    while (true) {
        json cmd;
        try {
            cmd = readMessage();
        } catch (const std::exception& e) {
            LOG_ERROR("[LlamaCppWorker] Read error: " << e.what());
            break;
        }

        std::string type = cmd.value("type", "");

        if (type == "EXECUTE") {
            handleExecute(sess, cmd);
        } else if (type == "RESET") {
            handleReset(sess, cmd);
        } else if (type == "SHUTDOWN") {
            LOG_INFO("[LlamaCppWorker] SHUTDOWN received — exiting");
            break;
        } else if (type == "SAVE_KV") {
            // KV save/restore not yet supported for llamacpp (MVP scope)
            std::string command_id = cmd.value("command_id", "");
            LOG_WARN("[LlamaCppWorker] SAVE_KV not supported — sending READY anyway");
            sendReady(command_id);
        } else if (type == "RESTORE_KV") {
            std::string command_id = cmd.value("command_id", "");
            LOG_WARN("[LlamaCppWorker] RESTORE_KV not supported — sending READY anyway");
            sendReady(command_id);
        } else {
            LOG_WARN("[LlamaCppWorker] Unknown command type: " << type);
        }
    }

    // Cleanup
#ifdef QAI_FORGE_LLAMACPP_MTMD
    if (sess.mtmd_ctx) {
        mtmd_free(sess.mtmd_ctx);
        sess.mtmd_ctx = nullptr;
    }
#endif
    sess.chat_templates.reset();
    if (sess.ctx) {
        llama_free(sess.ctx);
        sess.ctx = nullptr;
    }
    if (sess.model) {
        llama_model_free(sess.model);
        sess.model = nullptr;
    }

    LOG_INFO("[LlamaCppWorker] Exiting cleanly");
    return 0;
}
