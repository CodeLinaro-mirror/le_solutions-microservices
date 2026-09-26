// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/backend/QNNBackend.h"
#include "qai_forge/execution/QnnLaunchOptions.h"
#include "qai_forge/worker/PredictiveWorkerManager.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/Logger.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <utility>

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
                             const std::string& model_file) {
    // No placement plan supplied — build an automatic (backend-managed) plan
    // so the worker still receives a well-formed provider/binding_strength.
    qai_forge::execution::ExecutionPlan automatic_plan;
    automatic_plan.provider = "qnn";
    qai_forge::execution::ComponentPlacement component;
    component.target.provider = "qnn";
    component.target.semantics =
        qai_forge::execution::TargetSemantics::BackendAutomatic;
    component.target.discovery_mode =
        qai_forge::execution::DiscoveryMode::BackendManaged;
    component.target.binding_strength =
        qai_forge::execution::BindingStrength::Automatic;
    automatic_plan.components.push_back(std::move(component));
    initialize(model_id, model_file, automatic_plan,
               qai_forge::execution::RuntimeId{model_id, 0});
}

void QNNBackend::initialize(
    const qai_forge::execution::RuntimeLaunchRequest& request) {
    initialize(request.model_id, "", request.execution_plan,
               request.runtime_id);
}

void QNNBackend::initialize(
    const std::string& model_id,
    const std::string& requested_model_file,
    const qai_forge::execution::ExecutionPlan& plan,
    qai_forge::execution::RuntimeId runtime_id) {
    // Build INIT params from ModelConfigManager
    const auto mc = ModelConfigManager::getInstance().getModelConfig(model_id);
    if (!mc)
        throw std::runtime_error("QNNBackend: model not found: " + model_id);

    // Resolve absolute model file path — prefer a caller-supplied path
    std::string model_file = requested_model_file.empty()
        ? mc->config_file  // config_file holds the processed path
        : requested_model_file;
    if (model_file.empty())
        throw std::runtime_error("QNNBackend: no model_file for: " + model_id);

    // Read QNN-specific fields from metadata (stored in ModelConfig extensions)
    // For now, use environment variable overrides or defaults
    const char* backend_lib_env = std::getenv("QNN_BACKEND_LIB");
    const char* sys_lib_env     = std::getenv("QNN_SYS_LIB");

    std::string backend_lib = backend_lib_env ? backend_lib_env : "/usr/lib/libQnnHtp.so";
    std::string sys_lib     = sys_lib_env     ? sys_lib_env     : "/usr/lib/libQnnSystem.so";

    nlohmann::ordered_json init_params = {
        {"model_file",  model_file},
        {"backend_lib", backend_lib},
        {"sys_lib",     sys_lib},
        {"runtime_id", {
            {"model_id", runtime_id.model_id},
            {"replica_id", runtime_id.replica_id}
        }},
        {"provider", "qnn"}
    };

    const auto launch_options =
        qai_forge::execution::QnnLaunchOptions::toInitOptions(plan);
    for (const auto& [key, value] : launch_options.items()) {
        init_params[key] = value;
    }

    if (!shared_memory_) shared_memory_ = std::make_unique<PredictiveSharedMemory>(predictiveShmDirBytes());

    const bool has_exact_target =
        launch_options.contains("qnn") &&
        launch_options["qnn"].contains("device_id");
    const uint32_t expected_device_id =
        has_exact_target
            ? launch_options["qnn"]["device_id"].get<uint32_t>()
            : 0;
    const auto expected_runtime_id = runtime_id;

    // ready_validator runs on the worker's post-INIT READY (see
    // src/worker/qnn/main.cpp) and confirms it echoed back the same
    // runtime_id, provider=="qnn", and the achieved binding_strength
    // ("exact" when the plan requested a physical device_id, "automatic"
    // otherwise) — plus, for the exact case, that qnn.effective_device_id
    // matches the requested device_id. Throwing here fails startup.
    worker_->ensureWorkerRunning(
        model_id,
        init_params,
        {shared_memory_->fd(), shared_memory_->data(), shared_memory_->dirBytes()},
        [expected_runtime_id, has_exact_target, expected_device_id](const json& ready) {
            if (!ready.contains("runtime_id") ||
                !ready["runtime_id"].is_object() ||
                ready["runtime_id"].value("model_id", "") !=
                    expected_runtime_id.model_id ||
                ready["runtime_id"].value("replica_id", UINT64_MAX) !=
                    expected_runtime_id.replica_id) {
                throw std::runtime_error(
                    "QNNBackend: worker READY reported an unexpected runtime_id");
            }
            if (ready.value("provider", "") != "qnn") {
                throw std::runtime_error(
                    "QNNBackend: worker READY reported an unexpected provider");
            }

            const std::string expected_binding =
                has_exact_target ? "exact" : "automatic";
            if (ready.value("binding_strength", "") != expected_binding) {
                throw std::runtime_error(
                    "QNNBackend: worker READY reported an unexpected binding "
                    "strength");
            }

            if (has_exact_target) {
                if (!ready.contains("qnn") ||
                    !ready["qnn"].is_object() ||
                    ready["qnn"].value("requested_device_id", 0u) !=
                        expected_device_id ||
                    ready["qnn"].value("effective_device_id", 0u) !=
                        expected_device_id) {
                    throw std::runtime_error(
                        "QNNBackend: worker READY did not confirm requested "
                        "device_id");
                }
            }
        });
    current_plan_ = plan;
    current_runtime_id_ = runtime_id;
    has_current_plan_ = true;
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
        if (has_current_plan_ && current_model_id_ == request.model) {
            initialize(request.model, "", current_plan_, current_runtime_id_);
        } else {
            initialize(request.model, "");
        }
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
        current_plan_ = {};
        current_runtime_id_ = {};
        has_current_plan_ = false;
        current_model_id_.clear();
        throw;
    }
    worker_->clearSharedMemory();
    shared_memory_.reset();
    current_plan_ = {};
    current_runtime_id_ = {};
    has_current_plan_ = false;
    current_model_id_.clear();
}
