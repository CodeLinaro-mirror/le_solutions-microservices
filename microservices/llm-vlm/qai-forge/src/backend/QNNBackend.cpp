// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/backend/QNNBackend.h"
#include "qai_forge/worker/PredictiveWorkerManager.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/Logger.h"
#include <cstdlib>
#include <cstring>
#include <stdexcept>

// Default worker binary path — overridable via QNN_WORKER_BINARY env var
static const char* DEFAULT_QNN_WORKER = "/usr/local/bin/qnn-inference-worker";
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

QNNBackend::QNNBackend() {
    const char* binary = std::getenv("QNN_WORKER_BINARY");
    if (!binary) binary = DEFAULT_QNN_WORKER;

    worker_ = std::make_unique<PredictiveWorkerManager>(binary, "qnn");
}

QNNBackend& QNNBackend::getInstance() {
    static QNNBackend instance;
    return instance;
}

void QNNBackend::initialize(const std::string& model_id,
                             const std::string& /*model_file*/) {
    // Build INIT params from ModelConfigManager
    const ModelConfig* mc = ModelConfigManager::getInstance().getModelConfig(model_id);
    if (!mc)
        throw std::runtime_error("QNNBackend: model not found: " + model_id);

    // Resolve absolute model file path
    std::string model_file = mc->config_file;  // config_file holds the processed path
    if (model_file.empty())
        throw std::runtime_error("QNNBackend: no model_file for: " + model_id);

    // Read QNN-specific fields from metadata (stored in ModelConfig extensions)
    // For now, use environment variable overrides or defaults
    const char* backend_lib_env = std::getenv("QNN_BACKEND_LIB");
    const char* sys_lib_env     = std::getenv("QNN_SYS_LIB");

    std::string backend_lib = backend_lib_env ? backend_lib_env : "/usr/lib/libQnnHtp.so";
    std::string sys_lib     = sys_lib_env     ? sys_lib_env     : "/usr/lib/libQnnSystem.so";

    nlohmann::ordered_json init_params = {
        {"model_file",   model_file},
        {"backend_lib",  backend_lib},
        {"sys_lib",      sys_lib}
    };

    if (!shared_memory_) shared_memory_ = std::make_unique<PredictiveSharedMemory>(predictiveShmDirBytes());
    worker_->ensureWorkerRunning(model_id, init_params,
        {shared_memory_->fd(), shared_memory_->data(), shared_memory_->dirBytes()});
    current_model_id_ = model_id;

    LOG_INFO("[QNNBackend] Initialized model: " << model_id
             << " backend=" << backend_lib);
}

PredictiveExecuteRequest QNNBackend::prepareWorkerRequest(
    const SegmentedTensorInferenceRequest& request) {
    if (!shared_memory_) throw std::runtime_error("QNNBackend: shared memory is not initialized");
    PredictiveExecuteRequest result{request.model, request.request_id,
        nlohmann::ordered_json::array(), nlohmann::ordered_json::array()};
    size_t write_offset = 0;
    for (const auto& tensor : request.inputs) {
        const size_t offset = alignUp(write_offset, kShmAlign);
        const size_t len = tensor.byte_size;
        if (len == 0) throw std::runtime_error("QNNBackend: invalid tensor size");
        if (offset + len > shared_memory_->dirBytes())
            throw std::runtime_error("QNNBackend: batched input exceeds shared-memory capacity");
        uint8_t* dst = shared_memory_->data() + offset;
        size_t copied = 0;
        for (const auto& segment : tensor.segments) {
            if ((!segment.data && segment.size) || copied + segment.size > len)
                throw std::runtime_error("QNNBackend: invalid input segment");
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

TensorInferenceResponse QNNBackend::infer(
    const SegmentedTensorInferenceRequest& request) {
    // Ensure worker is running (auto-restart if crashed)
    if (!worker_->isWorkerRunning() || worker_->getCurrentModelId() != request.model) {
        initialize(request.model, "");
    }

    static int event_counter = 0;
    std::string event_id = "qnn-evt-" + std::to_string(++event_counter);

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
        throw std::runtime_error("QNNBackend::infer failed: " + error_msg);

    return result;
}

bool QNNBackend::isHealthy() const {
    return worker_->isWorkerRunning();
}

void QNNBackend::shutdown() {
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
