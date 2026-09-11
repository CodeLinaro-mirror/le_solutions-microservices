// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/backend/SNPEBackend.h"
#include "qai_forge/worker/PredictiveWorkerManager.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/Logger.h"
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <nlohmann/json.hpp>

// Default worker binary path — overridable via SNPE_WORKER_BINARY env var
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

SNPEBackend::SNPEBackend() {
    const char* binary = std::getenv("SNPE_WORKER_BINARY");
    if (!binary) binary = DEFAULT_SNPE_WORKER;

    worker_ = std::make_unique<PredictiveWorkerManager>(binary, "snpe");
}

SNPEBackend& SNPEBackend::getInstance() {
    static SNPEBackend instance;
    return instance;
}

void SNPEBackend::initialize(const std::string& model_id,
                              const std::string& /*model_file*/) {
    const ModelConfig* mc = ModelConfigManager::getInstance().getModelConfig(model_id);
    if (!mc)
        throw std::runtime_error("SNPEBackend: model not found: " + model_id);

    std::string model_file = mc->config_file;
    if (model_file.empty())
        throw std::runtime_error("SNPEBackend: no model_file for: " + model_id);

    // Delegate: read from env or default to "dsp"
    const char* delegate_env = std::getenv("SNPE_DELEGATE");
    std::string delegate = delegate_env ? delegate_env : "dsp";

    // Build INIT params
    nlohmann::ordered_json init_params = {
        {"model_file",     model_file},
        {"delegate",       delegate},
        {"output_tensors", nlohmann::ordered_json::array()}  // empty = all outputs
    };

    if (!shared_memory_) shared_memory_ = std::make_unique<PredictiveSharedMemory>(predictiveShmDirBytes());
    worker_->ensureWorkerRunning(model_id, init_params,
        {shared_memory_->fd(), shared_memory_->data(), shared_memory_->dirBytes()});
    current_model_id_ = model_id;

    LOG_INFO("[SNPEBackend] Initialized model: " << model_id
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
    if (!worker_->isWorkerRunning() || worker_->getCurrentModelId() != request.model) {
        initialize(request.model, "");
    }

    static int event_counter = 0;
    std::string event_id = "snpe-evt-" + std::to_string(++event_counter);

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
        throw std::runtime_error("SNPEBackend::infer failed: " + error_msg);

    return result;
}

bool SNPEBackend::isHealthy() const {
    return worker_->isWorkerRunning();
}

void SNPEBackend::shutdown() {
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
