// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/backend/SNPEBackend.h"
#include "qai_forge/worker/PredictiveWorkerManager.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/Logger.h"
#include <stdexcept>
#include <nlohmann/json.hpp>

// Default worker binary path — overridable via SNPE_WORKER_BINARY env var
static const char* DEFAULT_SNPE_WORKER = "/usr/local/bin/snpe-inference-worker";

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

    worker_->ensureWorkerRunning(model_id, init_params);
    current_model_id_ = model_id;

    LOG_INFO("[SNPEBackend] Initialized model: " << model_id
             << " delegate=" << delegate);
}

TensorInferenceResponse SNPEBackend::infer(const TensorInferenceRequest& request) {
    if (!worker_->isWorkerRunning() || worker_->getCurrentModelId() != request.model) {
        initialize(request.model, "");
    }

    static int event_counter = 0;
    std::string event_id = "snpe-evt-" + std::to_string(++event_counter);

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
        throw std::runtime_error("SNPEBackend::infer failed: " + error_msg);

    return result;
}

bool SNPEBackend::isHealthy() const {
    return worker_->isWorkerRunning();
}

void SNPEBackend::shutdown() {
    worker_->shutdown();
}
