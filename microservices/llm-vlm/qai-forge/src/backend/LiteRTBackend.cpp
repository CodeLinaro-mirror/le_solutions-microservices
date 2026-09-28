// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// LiteRTBackend — IInferenceBackend implementation for LiteRT Predictive AI
//
// Uses a shared PredictiveWorkerChannel (via PredictiveWorkerProcessPool) to
// manage the ONE litert-inference-worker process hosting ALL LiteRT-backed
// resident models. The worker binary path is resolved from the
// LITERT_WORKER_BINARY environment variable (default:
// /usr/local/bin/litert-inference-worker).
//
// LOAD_MODEL params sent to the worker:
//   model_file          — absolute path to the .tflite model
//   dispatch_lib_dir    — directory containing LiteRT dispatch .so
//   compiler_plugin_dir — directory containing LiteRT compiler plugin .so
//   hw_accelerators     — bitmask (NPU=4, CPU=1; default 5 = NPU|CPU)
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/backend/LiteRTBackend.h"
#include "qai_forge/execution/LiteRtLaunchOptions.h"
#include "qai_forge/worker/PredictiveWorkerProcessPool.h"
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

static std::string litertWorkerBinaryPath() {
    const char* binary = std::getenv("LITERT_WORKER_BINARY");
    return binary ? binary : DEFAULT_LITERT_WORKER;
}

LiteRTBackend::LiteRTBackend() {
    channel_ = &PredictiveWorkerProcessPool::getInstance().getChannel(
        litertWorkerBinaryPath(), "litert");
}

LiteRTBackend& LiteRTBackend::getInstance() {
    static LiteRTBackend instance;
    return instance;
}

void LiteRTBackend::initialize(const std::string& model_id,
                                const std::string& model_file) {
    qai_forge::execution::ExecutionPlan automatic_plan;
    automatic_plan.provider = "litert";
    qai_forge::execution::ComponentPlacement component;
    component.target.provider = "litert";
    component.target.semantics =
        qai_forge::execution::TargetSemantics::BackendAutomatic;
    component.target.discovery_mode =
        qai_forge::execution::DiscoveryMode::BackendManaged;
    component.target.binding_strength =
        qai_forge::execution::BindingStrength::Automatic;
    automatic_plan.components.push_back(std::move(component));
    initialize(model_id, model_file, automatic_plan);
}

void LiteRTBackend::initialize(
    const qai_forge::execution::RuntimeLaunchRequest& request) {
    initialize(request.model_id, "", request.execution_plan);
}

void LiteRTBackend::initialize(
    const std::string& model_id,
    const std::string& requested_model_file,
    const qai_forge::execution::ExecutionPlan& plan) {
    // Resolve model config from ModelConfigManager
    const auto mc = ModelConfigManager::getInstance().getModelConfig(model_id);
    if (!mc)
        throw std::runtime_error("LiteRTBackend: model not found: " + model_id);

    std::string model_file = requested_model_file.empty()
        ? mc->config_file
        : requested_model_file;
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

    const auto launch_options =
        qai_forge::execution::LiteRtLaunchOptions::toInitOptions(plan);
    for (const auto& [key, value] : launch_options.items()) {
        init_params[key] = value;
    }
    if (launch_options.contains("litert") &&
        launch_options["litert"].contains("hw_accelerators")) {
        hw_accelerators = launch_options["litert"]["hw_accelerators"].get<int>();
        init_params["hw_accelerators"] = hw_accelerators;
    }

    // Each model keeps its own independent shared-memory region — the
    // worker PROCESS is shared, but tensor transport memory is not.
    if (!shared_memory_) shared_memory_ = std::make_unique<PredictiveSharedMemory>(predictiveShmDirBytes());

    const int expected_accelerators = hw_accelerators;
    // ready_validator runs on the worker's post-LOAD_MODEL READY (see
    // src/worker/litert/main.cpp) and confirms provider=="litert",
    // binding_strength=="automatic" (LiteRT binding is always automatic —
    // there is no per-plan exact/preferred override today), and that
    // litert.hw_accelerators matches the mask sent in LOAD_MODEL. Throwing
    // here fails startup.
    channel_->loadModel(
        model_id,
        init_params,
        shared_memory_->fd(),
        shared_memory_->data(),
        shared_memory_->dirBytes(),
        [expected_accelerators](const json& ready) {
            if (ready.value("provider", "") != "litert") {
                throw std::runtime_error(
                    "LiteRTBackend: worker READY reported an unexpected provider");
            }
            if (ready.value("binding_strength", "") != "automatic") {
                throw std::runtime_error(
                    "LiteRTBackend: worker READY reported an unexpected binding "
                    "strength");
            }
            if (!ready.contains("litert") ||
                !ready["litert"].is_object() ||
                ready["litert"].value("hw_accelerators", -1) !=
                    expected_accelerators) {
                throw std::runtime_error(
                    "LiteRTBackend: worker READY did not confirm requested "
                    "accelerator classes");
            }
        });
    current_plan_ = plan;
    has_current_plan_ = true;
    current_model_id_ = model_id;

    LOG_INFO("[LiteRTBackend] Loaded model into shared worker process: " << model_id
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
    if (!channel_->isAlive() || !has_current_plan_ ||
        current_model_id_ != request.model) {
        if (has_current_plan_ && current_model_id_ == request.model) {
            initialize(request.model, "", current_plan_);
        } else {
            initialize(request.model, "");
        }
    }

    static int event_counter = 0;
    std::string event_id = "litert-evt-" + std::to_string(++event_counter);

    const PredictiveExecuteRequest worker_request = prepareWorkerRequest(request);

    json execute_message = {
        {"type",         "EXECUTE"},
        {"event_id",     event_id},
        {"model",        worker_request.model},
        {"inputs",       worker_request.inputs},
        {"output_names", worker_request.output_names}
    };

    const auto exec_result =
        channel_->executeInfer(request.model, event_id, execute_message);

    if (!exec_result.ok)
        throw std::runtime_error("LiteRTBackend::infer failed: " + exec_result.error_message);

    TensorInferenceResponse result;
    result.model      = request.model;
    result.request_id = request.request_id;

    for (const auto& t : exec_result.outputs) {
        OutputTensor out;
        out.name  = t.value("name", "");
        out.dtype = tensorDataTypeFromString(t.value("dtype", "FP32"));

        for (auto d : t.value("shape", json::array()))
            out.shape.push_back(d.get<int64_t>());

        const json& ref = t.at("data_ref");
        size_t offset = ref.value("offset", (size_t)0);
        size_t len    = ref.value("len", (size_t)0);
        if (offset + len > 2 * shared_memory_->dirBytes())
            throw std::runtime_error("output data_ref out of bounds");

        out.data.resize(len);
        std::memcpy(out.data.data(), shared_memory_->data() + offset, len);

        result.outputs.push_back(std::move(out));
    }

    return result;
}

bool LiteRTBackend::isHealthy() const {
    return channel_->isAlive();
}

void LiteRTBackend::shutdown() {
    // Only unload THIS model from the shared process — other LiteRT-backed
    // models resident in the same worker process are unaffected.
    if (has_current_plan_ && !current_model_id_.empty()) {
        channel_->unloadModel(current_model_id_);
    }
    shared_memory_.reset();
    current_plan_ = {};
    has_current_plan_ = false;
    current_model_id_.clear();
}
