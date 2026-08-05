// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/backend/QNNBackend.h"
#include "qai_forge/worker/ConventionalWorkerManager.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/Logger.h"
#include <stdexcept>

// Default worker binary path — overridable via QNN_WORKER_BINARY env var
static const char* DEFAULT_QNN_WORKER = "/usr/local/bin/qnn-inference-worker";

QNNBackend::QNNBackend() {
    const char* binary = std::getenv("QNN_WORKER_BINARY");
    if (!binary) binary = DEFAULT_QNN_WORKER;
    worker_ = std::make_unique<ConventionalWorkerManager>(binary, "qnn");
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

    worker_->ensureWorkerRunning(model_id, init_params);
    current_model_id_ = model_id;

    LOG_INFO("[QNNBackend] Initialized model: " << model_id
             << " backend=" << backend_lib);
}

TensorInferenceResponse QNNBackend::infer(const TensorInferenceRequest& request) {
    // Ensure worker is running (auto-restart if crashed)
    if (!worker_->isWorkerRunning() || worker_->getCurrentModelId() != request.model) {
        initialize(request.model, "");
    }

    static int event_counter = 0;
    std::string event_id = "qnn-evt-" + std::to_string(++event_counter);

    TensorInferenceResponse result;
    std::string error_msg;
    bool had_error = false;

    worker_->executeInfer(
        event_id,
        request,
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

    result.stats.backend_name = "QNN";
    return result;
}

bool QNNBackend::isHealthy() const {
    return worker_->isWorkerRunning();
}

void QNNBackend::shutdown() {
    worker_->shutdown();
}
