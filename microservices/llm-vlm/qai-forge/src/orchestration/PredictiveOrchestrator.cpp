// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/orchestration/PredictiveOrchestrator.h"

#include "qai_forge/backend/IInferenceBackend.h"
#include "qai_forge/managers/ModelConfigManager.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

namespace {

size_t elementWidth(TensorDataType dtype) {
    switch (dtype) {
        case TensorDataType::FLOAT32:
        case TensorDataType::INT32:
            return 4;
        case TensorDataType::FLOAT16:
        case TensorDataType::INT16:
            return 2;
        case TensorDataType::INT8:
        case TensorDataType::UINT8:
        case TensorDataType::BOOL:
            return 1;
        case TensorDataType::BYTES:
            return 0;
    }
    return 0;
}

bool shapeMatches(const std::vector<int64_t>& expected,
                  const std::vector<int64_t>& actual) {
    if (expected.size() != actual.size()) {
        return false;
    }
    for (size_t index = 0; index < expected.size(); ++index) {
        if (expected[index] > 0 && expected[index] != actual[index]) {
            return false;
        }
        if (actual[index] <= 0) {
            return false;
        }
    }
    return true;
}

size_t expectedByteCount(const InputTensor& input) {
    const size_t width = elementWidth(input.dtype);
    if (width == 0) {
        return input.data.size();
    }

    size_t elements = 1;
    for (const int64_t dimension : input.shape) {
        if (dimension <= 0 ||
            elements > std::numeric_limits<size_t>::max() /
                           static_cast<size_t>(dimension)) {
            throw GenAIException(
                GenAIErrorCode::INVALID_REQUEST,
                "Input tensor '" + input.name + "' has an invalid shape",
                400);
        }
        elements *= static_cast<size_t>(dimension);
    }
    if (elements > std::numeric_limits<size_t>::max() / width) {
        throw GenAIException(
            GenAIErrorCode::INVALID_REQUEST,
            "Input tensor '" + input.name + "' byte count overflows",
            400);
    }
    return elements * width;
}

const InputTensor* findInput(const TensorInferenceRequest& request,
                             const ModelTensorSpec& spec,
                             size_t index) {
    if (!spec.name.empty()) {
        const auto found = std::find_if(
            request.inputs.begin(),
            request.inputs.end(),
            [&spec](const InputTensor& input) {
                return input.name == spec.name;
            });
        return found == request.inputs.end() ? nullptr : &*found;
    }
    return index < request.inputs.size() ? &request.inputs[index] : nullptr;
}

void validateRequest(const scheduler::PredictiveJobContext& context) {
    const ModelConfig* stored_config =
        ModelConfigManager::getInstance().getModelConfig(context.model_id);
    if (!stored_config) {
        throw GenAIException(
            GenAIErrorCode::MODEL_NOT_FOUND,
            "Model '" + context.model_id + "' not found",
            404);
    }
    const ModelConfig config = *stored_config;
    if (config.model_type != "predictive") {
        throw GenAIException(
            GenAIErrorCode::INVALID_REQUEST,
            "Model '" + context.model_id + "' is not predictive",
            400);
    }

    if (context.request.inputs.size() != config.input_specs.size()) {
        throw GenAIException(
            GenAIErrorCode::INVALID_REQUEST,
            "Model '" + context.model_id + "' expects " +
                std::to_string(config.input_specs.size()) +
                " input tensors but received " +
                std::to_string(context.request.inputs.size()),
            400);
    }

    for (size_t index = 0; index < config.input_specs.size(); ++index) {
        const ModelTensorSpec& spec = config.input_specs[index];
        const InputTensor* input = findInput(context.request, spec, index);
        if (!input) {
            throw GenAIException(
                GenAIErrorCode::INVALID_REQUEST,
                "Missing input tensor '" + spec.name + "'",
                400);
        }
        if (!shapeMatches(spec.shape, input->shape)) {
            throw GenAIException(
                GenAIErrorCode::INVALID_REQUEST,
                "Input tensor '" + input->name + "' has the wrong shape",
                400);
        }
        const std::string actual_dtype = tensorDataTypeToString(input->dtype);
        if (spec.dtype != actual_dtype) {
            throw GenAIException(
                GenAIErrorCode::INVALID_REQUEST,
                "Input tensor '" + input->name + "' has dtype '" +
                    actual_dtype + "' but model expects '" + spec.dtype + "'",
                400);
        }
        const size_t expected_bytes = expectedByteCount(*input);
        if (input->data.size() != expected_bytes) {
            throw GenAIException(
                GenAIErrorCode::INVALID_REQUEST,
                "Input tensor '" + input->name + "' has " +
                    std::to_string(input->data.size()) +
                    " bytes but its shape requires " +
                    std::to_string(expected_bytes),
                400);
        }
    }

    if (!context.request.output_names.empty()) {
        if (config.output_specs.empty()) {
            throw GenAIException(
                GenAIErrorCode::INVALID_REQUEST,
                "Model '" + context.model_id +
                    "' does not declare output specs for named output selection",
                400);
        }
        std::unordered_set<std::string> known_outputs;
        for (const ModelTensorSpec& spec : config.output_specs) {
            known_outputs.insert(spec.name);
        }
        for (const std::string& output_name : context.request.output_names) {
            if (known_outputs.count(output_name) == 0) {
                throw GenAIException(
                    GenAIErrorCode::INVALID_REQUEST,
                    "Unknown output tensor '" + output_name + "' for model '" +
                        context.model_id + "'",
                    400);
            }
        }
    }
}

} // namespace

scheduler::PredictiveJobPtr PredictiveOrchestrator::createJob(
    scheduler::PredictiveJobContext context,
    scheduler::PredictiveCallbacks callbacks) const {
    validateRequest(context);

    auto job = std::make_shared<scheduler::PredictiveJob>();
    job->job_id = context.job_id;
    job->model_id = context.model_id;
    job->prepared = std::move(context.request);
    job->prepared.request_id = context.job_id;
    job->callbacks = std::move(callbacks);
    return job;
}

TensorInferenceResponse PredictiveOrchestrator::execute(
    scheduler::PredictiveJob& job,
    IInferenceBackend& backend) const {
    const auto started_at = std::chrono::steady_clock::now();
    TensorInferenceResponse response = backend.infer(job.prepared);
    const auto finished_at = std::chrono::steady_clock::now();

    response.model = job.model_id;
    response.request_id = job.job_id;
    response.stats.latency_ms =
        std::chrono::duration<double, std::milli>(finished_at - started_at)
            .count();
    response.stats.backend_name = backend.name();

    if (job.prepared.output_names.empty()) {
        return response;
    }

    std::unordered_set<std::string> requested(
        job.prepared.output_names.begin(),
        job.prepared.output_names.end());
    std::unordered_set<std::string> returned;
    std::vector<OutputTensor> filtered;
    filtered.reserve(response.outputs.size());
    for (OutputTensor& output : response.outputs) {
        if (requested.count(output.name) == 0) {
            continue;
        }
        returned.insert(output.name);
        filtered.push_back(std::move(output));
    }

    for (const std::string& output_name : job.prepared.output_names) {
        if (returned.count(output_name) == 0) {
            throw GenAIException(
                GenAIErrorCode::INFERENCE_FAILED,
                "Backend did not return requested output tensor '" +
                    output_name + "'",
                500);
        }
    }
    response.outputs = std::move(filtered);
    return response;
}
