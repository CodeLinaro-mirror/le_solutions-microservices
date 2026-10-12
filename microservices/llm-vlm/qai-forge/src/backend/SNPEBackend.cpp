// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/backend/SNPEBackend.h"
#include "qai_forge/execution/SnpeLaunchOptions.h"
#include "qai_forge/worker/PredictiveWorkerProcessPool.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/Logger.h"
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <nlohmann/json.hpp>

// Default worker binary path — overridable via SNPE_WORKER_BINARY env var.
// Shared by ALL SNPE-backed models — one process, many resident models
// multiplexed onto per-model executor threads inside it
// (see src/worker/snpe/main.cpp).
static const char* DEFAULT_SNPE_WORKER = "/usr/local/bin/snpe-inference-worker";
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

static std::string snpeWorkerBinaryPath() {
    const char* binary = std::getenv("SNPE_WORKER_BINARY");
    return binary ? binary : DEFAULT_SNPE_WORKER;
}

SNPEBackend::SNPEBackend() {
    channel_ = &PredictiveWorkerProcessPool::getInstance().getChannel(
        snpeWorkerBinaryPath(), "snpe");
}

SNPEBackend& SNPEBackend::getInstance() {
    static SNPEBackend instance;
    return instance;
}

void SNPEBackend::initialize(const std::string& model_id,
                              const std::string& model_file) {
    // No placement plan supplied — build an automatic RuntimePreference plan
    // seeded from SNPE_DELEGATE so the worker still sees a well-formed
    // provider/binding_strength.
    const char* delegate_env = std::getenv("SNPE_DELEGATE");
    std::string delegate = delegate_env ? delegate_env : "dsp";
    std::string runtime_order = "DSP,CPU";
    if (delegate == "aip") runtime_order = "AIP_FIXED8_TF,CPU";
    else if (delegate == "gpu") runtime_order = "GPU,CPU";
    else if (delegate == "none" || delegate == "cpu") runtime_order = "CPU";

    qai_forge::execution::ExecutionPlan automatic_plan;
    automatic_plan.provider = "snpe";
    qai_forge::execution::ComponentPlacement component;
    component.target.provider = "snpe";
    component.target.target_id = "runtime_order=" + runtime_order;
    component.target.semantics =
        qai_forge::execution::TargetSemantics::RuntimePreference;
    component.target.discovery_mode =
        qai_forge::execution::DiscoveryMode::Configured;
    component.target.binding_strength =
        qai_forge::execution::BindingStrength::Preferred;
    automatic_plan.components.push_back(std::move(component));
    initialize(model_id, model_file, automatic_plan);
}

void SNPEBackend::initialize(
    const qai_forge::execution::RuntimeLaunchRequest& request) {
    initialize(request.model_id, "", request.execution_plan);
}

void SNPEBackend::initialize(
    const std::string& model_id,
    const std::string& requested_model_file,
    const qai_forge::execution::ExecutionPlan& plan) {
    const auto mc = ModelConfigManager::getInstance().getModelConfig(model_id);
    if (!mc)
        throw std::runtime_error("SNPEBackend: model not found: " + model_id);

    std::string model_file = requested_model_file.empty()
        ? mc->config_file
        : requested_model_file;
    if (model_file.empty())
        throw std::runtime_error("SNPEBackend: no model_file for: " + model_id);

    std::string delegate =
        qai_forge::execution::SnpeLaunchOptions::runtimeOrder(plan);
    if (delegate.empty()) {
        const char* delegate_env = std::getenv("SNPE_DELEGATE");
        delegate = delegate_env ? delegate_env : "dsp";
    }

    // Output tensor names to force-expose via SNPEBuilderSetOutputTensors --
    // without this, SNPEEngine falls back to SNPE_GetOutputTensorNames()'s
    // own default output set once the interpreter is built, which for some
    // compiled .dlc graphs is a strict subset of what the model's config
    // declares (empty output_tensors is the "let SNPE decide" branch in
    // snpe-engine.cpp). Passing every declared name here forces the builder
    // to expose all of them, as long as they still exist by name in the
    // compiled graph.
    nlohmann::ordered_json output_tensors = nlohmann::ordered_json::array();
    for (const auto& spec : mc->output_specs) output_tensors.push_back(spec.name);

    // Build LOAD_MODEL params
    nlohmann::ordered_json init_params = {
        {"model_file",     model_file},
        {"delegate",       delegate},
        {"output_tensors", output_tensors}
    };

    const auto launch_options =
        qai_forge::execution::SnpeLaunchOptions::toInitOptions(plan);
    for (const auto& [key, value] : launch_options.items()) {
        init_params[key] = value;
    }

    // Each model keeps its own independent shared-memory region — the
    // worker PROCESS is shared, but tensor transport memory is not.
    if (!shared_memory_) shared_memory_ = std::make_unique<PredictiveSharedMemory>(predictiveShmDirBytes());

    const bool has_runtime_preference =
        launch_options.contains("snpe") &&
        launch_options["snpe"].contains("runtime_order");
    const std::string expected_runtime_order =
        has_runtime_preference
            ? launch_options["snpe"]["runtime_order"].get<std::string>()
            : std::string{};

    // ready_validator runs on the worker's post-LOAD_MODEL READY (see
    // src/worker/snpe/main.cpp) and confirms provider=="snpe" and the
    // achieved binding_strength ("preferred" when the plan specified a
    // runtime_order, "automatic" otherwise) — plus, when a runtime_order
    // was requested, that snpe.runtime_order matches what was sent in
    // LOAD_MODEL. Throwing here fails startup.
    channel_->loadModel(
        model_id,
        init_params,
        shared_memory_->fd(),
        shared_memory_->data(),
        shared_memory_->dirBytes(),
        [has_runtime_preference, expected_runtime_order](const json& ready) {
            if (ready.value("provider", "") != "snpe") {
                throw std::runtime_error(
                    "SNPEBackend: worker READY reported an unexpected provider");
            }

            const std::string expected_binding =
                has_runtime_preference ? "preferred" : "automatic";
            if (ready.value("binding_strength", "") != expected_binding) {
                throw std::runtime_error(
                    "SNPEBackend: worker READY reported binding_strength '" +
                    ready.value("binding_strength", std::string{}) +
                    "', expected '" + expected_binding + "'");
            }

            if (has_runtime_preference) {
                if (!ready.contains("snpe") ||
                    !ready["snpe"].is_object() ||
                    ready["snpe"].value("runtime_order", "") !=
                        expected_runtime_order) {
                    throw std::runtime_error(
                        "SNPEBackend: worker READY did not confirm requested "
                        "SNPE runtime order");
                }
            }
        });
    current_plan_ = plan;
    has_current_plan_ = true;
    current_model_id_ = model_id;

    LOG_INFO("[SNPEBackend] Loaded model into shared worker process: " << model_id
             << " delegate=" << delegate);
}

PredictiveExecuteRequest SNPEBackend::prepareWorkerRequest(
    const SegmentedTensorInferenceRequest& request) {
    if (!shared_memory_) throw std::runtime_error("SNPEBackend: shared memory is not initialized");
    PredictiveExecuteRequest result{request.model, request.request_id,
        nlohmann::ordered_json::array(), nlohmann::ordered_json::array()};
    size_t write_offset = 0;
    for (const auto& tensor : request.inputs) {
        const size_t offset = alignUp(write_offset, kShmAlign);
        const size_t len = tensor.byte_size;
        if (len == 0) throw std::runtime_error("SNPEBackend: invalid tensor size");
        if (offset + len > shared_memory_->dirBytes())
            throw std::runtime_error("SNPEBackend: batched input exceeds shared-memory capacity");
        uint8_t* dst = shared_memory_->data() + offset;
        size_t copied = 0;
        for (const auto& segment : tensor.segments) {
            if ((!segment.data && segment.size) || copied + segment.size > len)
                throw std::runtime_error("SNPEBackend: invalid input segment");
            std::memcpy(dst + copied, segment.data, segment.size);
            copied += segment.size;
        }
        std::memset(dst + copied, tensor.padding_value, len - copied);
        result.inputs.push_back({
            {"name", tensor.name},
            {"dtype", tensorDataTypeToString(tensor.dtype)},
            {"data_ref", {{"offset", offset}, {"len", len}}},
            {"shape", tensor.shape}});
        write_offset = offset + len;
    }
    for (const auto& name : request.output_names) result.output_names.push_back(name);
    return result;
}

TensorInferenceResponse SNPEBackend::infer(
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
    std::string event_id = "snpe-evt-" + std::to_string(++event_counter);

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
        throw std::runtime_error("SNPEBackend::infer failed: " + exec_result.error_message);

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

bool SNPEBackend::isHealthy() const {
    return channel_->isAlive();
}

void SNPEBackend::shutdown() {
    // Only unload THIS model from the shared process — other SNPE-backed
    // models resident in the same worker process are unaffected.
    if (has_current_plan_ && !current_model_id_.empty()) {
        channel_->unloadModel(current_model_id_);
    }
    shared_memory_.reset();
    current_plan_ = {};
    has_current_plan_ = false;
    current_model_id_.clear();
}
