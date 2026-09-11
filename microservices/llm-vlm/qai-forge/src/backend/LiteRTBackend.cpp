// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// LiteRTBackend — IInferenceBackend implementation for LiteRT Predictive AI
//
// Manages the litert-inference-worker subprocess via PredictiveWorkerManager.
// The worker binary path is resolved from the LITERT_WORKER_BINARY environment
// variable (default: /usr/local/bin/litert-inference-worker).
//
// INIT params sent to the worker:
//   model_file          — absolute path to the .tflite model
//   dispatch_lib_dir    — directory containing LiteRT dispatch .so
//   compiler_plugin_dir — directory containing LiteRT compiler plugin .so
//   hw_accelerators     — bitmask (NPU=4, CPU=1; default 5 = NPU|CPU)
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/backend/LiteRTBackend.h"
#include "qai_forge/worker/PredictiveWorkerManager.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/Logger.h"
#include <cstdlib>
#include <cstring>
#include <stdexcept>

// Default worker binary path — overridable via LITERT_WORKER_BINARY env var
static const char* DEFAULT_LITERT_WORKER = "/usr/local/bin/litert-inference-worker";

// Default library directories — overridable via env vars
static const char* DEFAULT_DISPATCH_DIR        = "/usr/lib";
static const char* DEFAULT_COMPILER_PLUGIN_DIR = "/usr/lib";
constexpr size_t kDefaultShmDirBytes = 32u * 1024 * 1024;
constexpr size_t kShmAlign = 128;

static size_t predictiveShmDirBytes() {
    if (const char* env = std::getenv("GENAI_PREDICTIVE_SHM_BYTES")) {
        const size_t bytes = std::strtoull(env, nullptr, 10);
        if (bytes > 0) return bytes;
    }
    return kDefaultShmDirBytes;
}

static size_t alignUp(size_t offset, size_t align) {
    return (offset + align - 1) / align * align;
}

LiteRTBackend::LiteRTBackend() {
    const char* binary = std::getenv("LITERT_WORKER_BINARY");
    if (!binary) binary = DEFAULT_LITERT_WORKER;

    worker_ = std::make_unique<PredictiveWorkerManager>(binary, "litert");
}

LiteRTBackend& LiteRTBackend::getInstance() {
    static LiteRTBackend instance;
    return instance;
}

void LiteRTBackend::initialize(const std::string& model_id,
                                const std::string& /*model_file*/) {
    // Resolve model config from ModelConfigManager
    const ModelConfig* mc = ModelConfigManager::getInstance().getModelConfig(model_id);
    if (!mc)
        throw std::runtime_error("LiteRTBackend: model not found: " + model_id);

    std::string model_file = mc->config_file;
    if (model_file.empty())
        throw std::runtime_error("LiteRTBackend: no model_file for: " + model_id);

    // Resolve library directories from env vars or defaults
    const char* dispatch_env = std::getenv("LITERT_DISPATCH_DIR");
    const char* plugin_env   = std::getenv("LITERT_COMPILER_PLUGIN_DIR");

    std::string dispatch_lib_dir    = dispatch_env ? dispatch_env : DEFAULT_DISPATCH_DIR;
    std::string compiler_plugin_dir = plugin_env   ? plugin_env   : DEFAULT_COMPILER_PLUGIN_DIR;

    // hw_accelerators: NPU(4) | CPU(1) = 5 by default
    // Can be overridden via LITERT_HW_ACCELERATORS env var
    int hw_accelerators = 5;
    const char* hw_env = std::getenv("LITERT_HW_ACCELERATORS");
    if (hw_env) {
        try { hw_accelerators = std::stoi(hw_env); }
        catch (...) { /* keep default */ }
    }

    nlohmann::ordered_json init_params = {
        {"model_file",          model_file},
        {"dispatch_lib_dir",    dispatch_lib_dir},
        {"compiler_plugin_dir", compiler_plugin_dir},
        {"hw_accelerators",     hw_accelerators}
    };

    if (!shared_memory_) shared_memory_ = std::make_unique<PredictiveSharedMemory>(predictiveShmDirBytes());
    worker_->ensureWorkerRunning(model_id, init_params,
        {shared_memory_->fd(), shared_memory_->data(), shared_memory_->dirBytes()});
    current_model_id_ = model_id;

    LOG_INFO("[LiteRTBackend] Initialized model: " << model_id
             << " dispatch=" << dispatch_lib_dir
             << " plugin=" << compiler_plugin_dir
             << " hw_accel=" << hw_accelerators);
}

PredictiveExecuteRequest LiteRTBackend::prepareWorkerRequest(
    const SegmentedTensorInferenceRequest& request) {
    if (!shared_memory_) {
        throw std::runtime_error("LiteRTBackend: shared memory is not initialized");
    }
    PredictiveExecuteRequest worker_request{
        request.model, request.request_id,
        nlohmann::ordered_json::array(), nlohmann::ordered_json::array()};
    size_t write_offset = 0;
    for (const auto& tensor : request.inputs) {
        const size_t offset = alignUp(write_offset, kShmAlign);
        const size_t len = tensor.byte_size;
        if (len == 0) throw std::runtime_error("LiteRTBackend: invalid batched tensor size");
        if (offset + len > shared_memory_->dirBytes()) {
            throw std::runtime_error("LiteRTBackend: batched input exceeds shared-memory capacity");
        }
        uint8_t* destination = shared_memory_->data() + offset;
        size_t copied = 0;
        for (const auto& segment : tensor.segments) {
            if ((!segment.data && segment.size) || copied + segment.size > len) {
                throw std::runtime_error("LiteRTBackend: invalid batched input segment");
            }
            std::memcpy(destination + copied, segment.data, segment.size);
            copied += segment.size;
        }
        std::memset(destination + copied, tensor.padding_value, len - copied);
        nlohmann::ordered_json input = {
            {"name", tensor.name},
            {"dtype", tensorDataTypeToString(tensor.dtype)},
            {"data_ref", {{"offset", offset}, {"len", len}}},
            {"shape", tensor.shape}};
        worker_request.inputs.push_back(std::move(input));
        write_offset = offset + len;
    }
    for (const auto& name : request.output_names) worker_request.output_names.push_back(name);
    return worker_request;
}

TensorInferenceResponse LiteRTBackend::infer(
    const SegmentedTensorInferenceRequest& request) {
    if (!worker_->isWorkerRunning() || worker_->getCurrentModelId() != request.model) {
        initialize(request.model, "");
    }

    static int event_counter = 0;
    std::string event_id = "litert-evt-" + std::to_string(++event_counter);

    TensorInferenceResponse result;
    std::string error_msg;
    bool had_error = false;

    const PredictiveExecuteRequest worker_request = prepareWorkerRequest(request);
    worker_->executeInfer(
        event_id,
        worker_request,
        [&result](const TensorInferenceResponse& r) {
            result = r;
        },
        [&had_error, &error_msg](const std::string& msg) {
            had_error = true;
            error_msg = msg;
        }
    );

    if (had_error)
        throw std::runtime_error("LiteRTBackend::infer failed: " + error_msg);

    return result;
}

bool LiteRTBackend::isHealthy() const {
    return worker_->isWorkerRunning();
}

void LiteRTBackend::shutdown() {
    try { worker_->shutdown(); }
    catch (...) {
        try { worker_->terminateWorker(true); } catch (...) {}
        worker_->clearSharedMemory();
        shared_memory_.reset();
        current_model_id_.clear();
        throw;
    }
    worker_->clearSharedMemory();
    shared_memory_.reset();
    current_model_id_.clear();
}
