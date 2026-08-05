// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// vlm-engine.cpp — Layer 4 VLM Engine Implementation
//
// Replaces vlm-service.cpp + vlm-interface.cpp.
//
// Key simplifications vs. the old implementation:
//   1. No Response/Message/Choices structs — tokens are plain std::string.
//   2. No streaming/non-streaming split — always streams via TokenCallback.
//      Layer 3 accumulates tokens when the upstream request is non-streaming.
//   3. No C interface — Layer 3 uses VlmEngine directly.
//   4. Internal GenIE handles are hidden in VlmEngine::Impl (Pimpl idiom).
// ─────────────────────────────────────────────────────────────────────────────

#include "vlm-engine.hpp"
#include "GeniePipeline.h"
#include "GenieNode.h"
#include "GenieLog.h"
#include "GenieSampler.h"

#include <nlohmann/json.hpp>
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
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>

using json = nlohmann::json;

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────
namespace {

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
    std::fprintf(stdout, "[GenIE-VLM] [%s] %s\n", levelStr, buf);
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

static GenieNode_IOName_t stringToNodeIO(const std::string& s) {
    static const std::unordered_map<std::string, GenieNode_IOName_t> kMap = {
        {"GENIE_NODE_TEXT_GENERATOR_TEXT_INPUT",             GENIE_NODE_TEXT_GENERATOR_TEXT_INPUT},
        {"GENIE_NODE_TEXT_GENERATOR_EMBEDDING_INPUT",        GENIE_NODE_TEXT_GENERATOR_EMBEDDING_INPUT},
        {"GENIE_NODE_TEXT_GENERATOR_TEXT_OUTPUT",            GENIE_NODE_TEXT_GENERATOR_TEXT_OUTPUT},
        {"GENIE_NODE_TEXT_ENCODER_TEXT_INPUT",               GENIE_NODE_TEXT_ENCODER_TEXT_INPUT},
        {"GENIE_NODE_TEXT_ENCODER_EMBEDDING_OUTPUT",         GENIE_NODE_TEXT_ENCODER_EMBEDDING_OUTPUT},
        {"GENIE_NODE_IMAGE_ENCODER_IMAGE_INPUT",             GENIE_NODE_IMAGE_ENCODER_IMAGE_INPUT},
        {"GENIE_NODE_IMAGE_ENCODER_EMBEDDING_OUTPUT",        GENIE_NODE_IMAGE_ENCODER_EMBEDDING_OUTPUT},
        {"GENIE_NODE_IMAGE_ENCODER_IMAGE_POS_SIN",           GENIE_NODE_IMAGE_ENCODER_IMAGE_POS_SIN},
        {"GENIE_NODE_IMAGE_ENCODER_IMAGE_POS_COS",           GENIE_NODE_IMAGE_ENCODER_IMAGE_POS_COS},
        {"GENIE_NODE_IMAGE_ENCODER_IMAGE_FULL_ATTN_MASK",   GENIE_NODE_IMAGE_ENCODER_IMAGE_FULL_ATTN_MASK},
        {"GENIE_NODE_IMAGE_ENCODER_IMAGE_WINDOW_ATTN_MASK", GENIE_NODE_IMAGE_ENCODER_IMAGE_WINDOW_ATTN_MASK},
    };
    auto it = kMap.find(s);
    if (it != kMap.end()) return it->second;
    throw std::invalid_argument("[VlmEngine] Unknown node IO: " + s);
}

// ── Per-request callback context ──────────────────────────────────────────────
struct VlmCallbackCtx {
    TokenCallback*      callback = nullptr;
    std::mutex          mtx;
    std::condition_variable cv;
    bool                done = false;
};

static Genie_Status_t vlmTextCallback(
    const char* responseStr,
    GenieNode_TextOutput_SentenceCode_t sentenceCode,
    const void* userData) {
    auto* ctx = static_cast<VlmCallbackCtx*>(const_cast<void*>(userData));
    if (!ctx || !ctx->callback) return GENIE_STATUS_ERROR_INVALID_ARGUMENT;

    bool is_end = (sentenceCode == GENIE_NODE_SENTENCE_END    ||
                   sentenceCode == GENIE_NODE_SENTENCE_ABORT  ||
                   sentenceCode == GENIE_NODE_SENTENCE_COMPLETE);

    std::string token = responseStr ? responseStr : "";

    if (is_end) {
        (*ctx->callback)(token, "stop");
        {
            std::lock_guard<std::mutex> lock(ctx->mtx);
            ctx->done = true;
        }
        ctx->cv.notify_one();
    } else if (!token.empty()) {
        (*ctx->callback)(token, "");
    }

    return GENIE_STATUS_SUCCESS;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// VlmEngine::Impl — owns all GenIE handles and model config
// ─────────────────────────────────────────────────────────────────────────────
struct VlmEngine::Impl {
    GenieLog_Handle_t      log_handle            = nullptr;
    GeniePipeline_Handle_t pipeline_handle        = nullptr;
    GenieNode_Handle_t     image_encoder_node     = nullptr;
    GenieNode_Handle_t     lut_encoder_node       = nullptr;
    GenieNode_Handle_t     text_generator_node    = nullptr;
    GenieSampler_Handle_t  sampler_handle         = nullptr;  // lazy-initialized
    std::string            sampler_config_path;
    std::string            model_id;

    struct CustomInput {
        std::string node;
        std::string input_type;
        std::string file;
    };

    struct ModelConfig {
        std::string imageEncoderConfig;
        std::string lutEncoderConfig;
        std::string textGeneratorConfig;
        std::string visionStartToken = "<|vision_start|>";
        std::string visionEndToken   = "<|vision_end|>";
        std::vector<CustomInput> custom_inputs;
    } model_config;

    // Static buffers for position embeddings etc. — loaded once, reused per request
    std::vector<std::shared_ptr<void>> static_buffers;
    std::vector<size_t>                static_buffer_sizes;

    ~Impl() {
        if (text_generator_node) GenieNode_free(text_generator_node);
        if (lut_encoder_node)    GenieNode_free(lut_encoder_node);
        if (image_encoder_node)  GenieNode_free(image_encoder_node);
        if (pipeline_handle)     GeniePipeline_free(pipeline_handle);
        if (log_handle)          GenieLog_free(log_handle);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// VlmEngine — constructor
// ─────────────────────────────────────────────────────────────────────────────
VlmEngine::VlmEngine(const std::string& model_id,
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
            throw std::runtime_error("[VlmEngine] Failed to create log handle");
    }

    // ── Load model config from JSON file ──────────────────────────────────────
    {
        std::ifstream f(config_path);
        if (!f.is_open())
            throw std::runtime_error("[VlmEngine] Failed to open config: " + config_path);
        json root = json::parse(f);
        if (root.empty())
            throw std::runtime_error("[VlmEngine] Empty config file: " + config_path);

        std::string model_key = root.begin().key();
        const auto& mc = root[model_key];

        const auto& nodes = mc.at("pipeline").at("nodes");
        m_impl->model_config.imageEncoderConfig  = nodes.at("imageEncoder").get<std::string>();
        m_impl->model_config.lutEncoderConfig    = nodes.at("lutEncoder").get<std::string>();
        m_impl->model_config.textGeneratorConfig = nodes.at("textGenerator").get<std::string>();

        if (mc.contains("vision_start_token"))
            m_impl->model_config.visionStartToken = mc["vision_start_token"].get<std::string>();
        if (mc.contains("vision_end_token"))
            m_impl->model_config.visionEndToken = mc["vision_end_token"].get<std::string>();

        if (mc.contains("custom_inputs") && mc["custom_inputs"].is_array()) {
            for (const auto& ci : mc["custom_inputs"]) {
                Impl::CustomInput c;
                c.node       = ci.at("node").get<std::string>();
                c.input_type = ci.at("input_type").get<std::string>();
                c.file       = ci.at("file").get<std::string>();
                m_impl->model_config.custom_inputs.push_back(std::move(c));
            }
        }
    }

    // ── Pipeline ──────────────────────────────────────────────────────────────
    {
        GeniePipelineConfig_Handle_t pipeline_cfg = nullptr;
        // Empty JSON config — logger is bound separately
        int32_t status = GeniePipelineConfig_createFromJson("{}", &pipeline_cfg);
        if (status != GENIE_STATUS_SUCCESS || !pipeline_cfg)
            throw std::runtime_error("[VlmEngine] Failed to create pipeline config");
        GeniePipelineConfig_bindLogger(pipeline_cfg, m_impl->log_handle);
        status = GeniePipeline_create(pipeline_cfg, &m_impl->pipeline_handle);
        GeniePipelineConfig_free(pipeline_cfg);
        if (status != GENIE_STATUS_SUCCESS || !m_impl->pipeline_handle)
            throw std::runtime_error("[VlmEngine] Failed to create pipeline");
    }

    // ── Helper: create a node from a JSON config file ─────────────────────────
    auto createNode = [&](const std::string& cfg_path) -> GenieNode_Handle_t {
        std::ifstream f(cfg_path);
        if (!f.is_open())
            throw std::runtime_error("[VlmEngine] Failed to open node config: " + cfg_path);
        std::string cfg_json((std::istreambuf_iterator<char>(f)), {});

        GenieNodeConfig_Handle_t node_cfg = nullptr;
        int32_t status = GenieNodeConfig_createFromJson(cfg_json.c_str(), &node_cfg);
        if (status != GENIE_STATUS_SUCCESS || !node_cfg)
            throw std::runtime_error("[VlmEngine] Failed to create node config: " + cfg_path);
        GenieNodeConfig_bindLogger(node_cfg, m_impl->log_handle);

        GenieNode_Handle_t node = nullptr;
        status = GenieNode_create(node_cfg, &node);
        GenieNodeConfig_free(node_cfg);
        if (status != GENIE_STATUS_SUCCESS || !node)
            throw std::runtime_error("[VlmEngine] Failed to create node: " + cfg_path);
        return node;
    };

    // ── Create nodes ──────────────────────────────────────────────────────────
    m_impl->image_encoder_node  = createNode(m_impl->model_config.imageEncoderConfig);
    m_impl->lut_encoder_node    = createNode(m_impl->model_config.lutEncoderConfig);
    m_impl->text_generator_node = createNode(m_impl->model_config.textGeneratorConfig);
    std::cout << "[VlmEngine] Nodes created" << std::endl;

    // ── Add nodes to pipeline ─────────────────────────────────────────────────
    GeniePipeline_addNode(m_impl->pipeline_handle, m_impl->image_encoder_node);
    GeniePipeline_addNode(m_impl->pipeline_handle, m_impl->lut_encoder_node);
    GeniePipeline_addNode(m_impl->pipeline_handle, m_impl->text_generator_node);

    // ── Connect nodes ─────────────────────────────────────────────────────────
    GeniePipeline_connect(m_impl->pipeline_handle,
        m_impl->image_encoder_node, GENIE_NODE_IMAGE_ENCODER_EMBEDDING_OUTPUT,
        m_impl->text_generator_node, GENIE_NODE_TEXT_GENERATOR_EMBEDDING_INPUT);
    GeniePipeline_connect(m_impl->pipeline_handle,
        m_impl->lut_encoder_node, GENIE_NODE_TEXT_ENCODER_EMBEDDING_OUTPUT,
        m_impl->text_generator_node, GENIE_NODE_TEXT_GENERATOR_EMBEDDING_INPUT);

    // ── Register text output callback (set once; userData passed per-execute) ─
    GenieNode_setTextCallback(m_impl->text_generator_node,
                               GENIE_NODE_TEXT_GENERATOR_TEXT_OUTPUT,
                               vlmTextCallback);

    // ── Load static custom inputs (position embeddings etc.) ─────────────────
    // Buffers are loaded once and reused across requests via setData each call.
    for (const auto& ci : m_impl->model_config.custom_inputs) {
        std::ifstream f(ci.file, std::ios::binary | std::ios::ate);
        if (!f)
            throw std::runtime_error("[VlmEngine] Failed to open custom input: " + ci.file);
        size_t file_size = static_cast<size_t>(f.tellg());

        void* raw_ptr = nullptr;
        if (posix_memalign(&raw_ptr, 4096, file_size) != 0)
            raw_ptr = malloc(file_size);
        if (!raw_ptr)
            throw std::runtime_error("[VlmEngine] Failed to allocate buffer for: " + ci.file);

        std::shared_ptr<void> buf(raw_ptr, free);
        f.seekg(0);
        f.read(static_cast<char*>(buf.get()), file_size);
        m_impl->static_buffers.push_back(buf);
        m_impl->static_buffer_sizes.push_back(file_size);

        GenieNode_Handle_t target =
            (ci.node == "imageEncoder")  ? m_impl->image_encoder_node  :
            (ci.node == "lutEncoder")    ? m_impl->lut_encoder_node    :
            (ci.node == "textGenerator") ? m_impl->text_generator_node : nullptr;
        if (!target)
            throw std::invalid_argument("[VlmEngine] Unknown node in custom_inputs: " + ci.node);

        GenieNode_IOName_t io = stringToNodeIO(ci.input_type);
        GenieNode_setData(target, io, buf.get(), file_size, nullptr);
        std::cout << "[VlmEngine] Loaded " << file_size << " bytes for " << ci.input_type << std::endl;
    }

    std::cout << "[VlmEngine] Model loaded: " << model_id << std::endl;
}

VlmEngine::~VlmEngine() = default;

// ─────────────────────────────────────────────────────────────────────────────
// VlmEngine::generate
// ─────────────────────────────────────────────────────────────────────────────
void VlmEngine::generate(const std::string& prompt,
                          const std::vector<ImageBuffer>& images,
                          const GenerationConfig& config,
                          TokenCallback callback) {
    // ── Apply sampler config if non-default ───────────────────────────────────
    if (config.temperature != 1.0f || config.top_p != 1.0f || config.top_k > 0 ||
        config.presence_penalty != 0.0f || config.frequency_penalty != 0.0f) {
        if (!m_impl->sampler_handle)
            GenieNode_getSampler(m_impl->text_generator_node, &m_impl->sampler_handle);

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

    // ── Re-apply static custom inputs (required before each execute) ──────────
    for (size_t i = 0; i < m_impl->model_config.custom_inputs.size(); ++i) {
        if (i >= m_impl->static_buffers.size()) break;
        const auto& ci = m_impl->model_config.custom_inputs[i];
        GenieNode_Handle_t target =
            (ci.node == "imageEncoder")  ? m_impl->image_encoder_node  :
            (ci.node == "lutEncoder")    ? m_impl->lut_encoder_node    :
            (ci.node == "textGenerator") ? m_impl->text_generator_node : nullptr;
        if (!target) continue;
        GenieNode_IOName_t io = stringToNodeIO(ci.input_type);
        GenieNode_setData(target, io,
                          m_impl->static_buffers[i].get(),
                          m_impl->static_buffer_sizes[i],
                          nullptr);
    }

    // ── Set image data ────────────────────────────────────────────────────────
    // Keep a local copy alive for the duration of the call.
    std::vector<uint8_t> image_copy;
    if (!images.empty() && images[0].data && images[0].size > 0) {
        const auto& img = images[0];  // VLM currently supports one image
        image_copy.assign(img.data, img.data + img.size);
        GenieNode_setData(m_impl->image_encoder_node,
                          GENIE_NODE_IMAGE_ENCODER_IMAGE_INPUT,
                          image_copy.data(),
                          image_copy.size(),
                          nullptr);
    }

    // ── Set text prompt ───────────────────────────────────────────────────────
    const std::string& vStart = m_impl->model_config.visionStartToken;
    const std::string& vEnd   = m_impl->model_config.visionEndToken;
    size_t startPos = prompt.find(vStart);
    size_t endPos   = prompt.find(vEnd);

    if (!images.empty() &&
        startPos != std::string::npos &&
        endPos   != std::string::npos &&
        endPos > startPos) {
        // Interleaved path: pre-vision text → image → post-vision text
        std::string preText  = prompt.substr(0, startPos + vStart.size());
        std::string postText = prompt.substr(endPos);
        GenieNode_setData(m_impl->lut_encoder_node, GENIE_NODE_TEXT_ENCODER_TEXT_INPUT,
                          preText.c_str(), preText.size(), nullptr);
        GenieNode_setData(m_impl->lut_encoder_node, GENIE_NODE_TEXT_ENCODER_TEXT_INPUT,
                          postText.c_str(), postText.size(), nullptr);
    } else {
        // Non-interleaved / text-only path
        GenieNode_setData(m_impl->lut_encoder_node, GENIE_NODE_TEXT_ENCODER_TEXT_INPUT,
                          prompt.c_str(), prompt.size(), nullptr);
    }

    // ── Execute pipeline ──────────────────────────────────────────────────────
    VlmCallbackCtx ctx;
    ctx.callback = &callback;
    ctx.done = false;

    Genie_Status_t exec_status = GENIE_STATUS_SUCCESS;
    try {
        exec_status = GeniePipeline_execute(m_impl->pipeline_handle, &ctx);
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(ctx.mtx);
            ctx.done = true;
        }
        ctx.cv.notify_one();
        throw;
    }

    // Wait for the callback to signal completion
    {
        std::unique_lock<std::mutex> lock(ctx.mtx);
        ctx.cv.wait(lock, [&ctx]{ return ctx.done; });
    }

    // Allow SDK callback threads to fully complete before resetting
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Reset pipeline state for next request
    GeniePipeline_reset(m_impl->pipeline_handle);
    std::this_thread::sleep_for(std::chrono::seconds(1));

    if (exec_status != GENIE_STATUS_SUCCESS &&
        exec_status != GENIE_STATUS_WARNING_CONTEXT_EXCEEDED) {
        throw std::runtime_error("[VlmEngine] Pipeline execution failed: " +
                                  std::to_string(exec_status));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// VlmEngine — KV cache
// ─────────────────────────────────────────────────────────────────────────────
void VlmEngine::save_kv(const std::string& name) {
    Genie_Status_t status = GeniePipeline_save(m_impl->pipeline_handle, name.c_str());
    if (status != GENIE_STATUS_SUCCESS)
        throw std::runtime_error("[VlmEngine] Failed to save KV cache: " + name);
}

void VlmEngine::restore_kv(const std::string& name) {
    Genie_Status_t status = GeniePipeline_restore(m_impl->pipeline_handle, name.c_str());
    if (status != GENIE_STATUS_SUCCESS)
        throw std::runtime_error("[VlmEngine] Failed to restore KV cache: " + name);
}
