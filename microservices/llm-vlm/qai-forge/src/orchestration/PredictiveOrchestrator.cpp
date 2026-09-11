// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/orchestration/PredictiveOrchestrator.h"

#include "qai_forge/backend/IInferenceBackend.h"
#include "qai_forge/managers/ModelConfigManager.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <optional>
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

uint8_t paddingValue(const ModelTensorSpec& spec, TensorDataType dtype) {
    if (dtype == TensorDataType::UINT8) {
        return static_cast<uint8_t>(
            std::max(0, std::min(255, spec.quant_zero_point)));
    }

    if (dtype == TensorDataType::INT8) {
        const int value =
            std::max(-128, std::min(127, spec.quant_zero_point));
        return static_cast<uint8_t>(static_cast<int8_t>(value));
    }

    return 0;
}

bool shapeMatches(const std::vector<int64_t>& expected,
                  const std::vector<int64_t>& actual) {
    if (expected.size() != actual.size()) {
        return false;
    }
    for (size_t index = 0; index < expected.size(); ++index) {
        if (actual[index] <= 0) {
            return false;
        }

        // The leading dimension is the batch dimension. A model compiled for
        // batch N accepts client batches in [1, N]; every other fixed
        // dimension must match exactly. Non-positive expected dimensions
        // continue to represent dynamic dimensions.
        if (index == 0 && expected[index] > 0) {
            if (actual[index] > expected[index]) {
                return false;
            }
        } else if (expected[index] > 0 && expected[index] != actual[index]) {
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

    std::optional<int64_t> request_batch_size;
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
        if (!input->shape.empty()) {
            const int64_t batch_size = input->shape.front();
            if (request_batch_size.has_value() &&
                request_batch_size.value() != batch_size) {
                throw GenAIException(
                    GenAIErrorCode::INVALID_REQUEST,
                    "Input tensor '" + input->name + "' has batch size " +
                        std::to_string(batch_size) +
                        " but previous inputs use batch size " +
                        std::to_string(request_batch_size.value()),
                    400);
            }
            request_batch_size = batch_size;
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

void normalizeRequest(scheduler::PredictiveJobContext& context) {
    const ModelConfig* config = ModelConfigManager::getInstance().getModelConfig(context.model_id);
    if (!config) return;
    std::vector<InputTensor> ordered;
    ordered.reserve(config->input_specs.size());
    for (size_t index = 0; index < config->input_specs.size(); ++index) {
        const ModelTensorSpec& spec = config->input_specs[index];
        if (!spec.name.empty()) {
            const auto input = std::find_if(
                context.request.inputs.begin(), context.request.inputs.end(),
                [&spec](const InputTensor& value) { return value.name == spec.name; });
            if (input != context.request.inputs.end())
                ordered.push_back(std::move(*input));
        } else if (index < context.request.inputs.size()) {
            ordered.push_back(std::move(context.request.inputs[index]));
        }
    }
    context.request.inputs = std::move(ordered);
}

} // namespace

scheduler::PredictiveJobPtr PredictiveOrchestrator::createJob(
    scheduler::PredictiveJobContext context,
    scheduler::PredictiveCallbacks callbacks) const {
    validateRequest(context);
    normalizeRequest(context);

    auto job = std::make_shared<scheduler::PredictiveJob>();
    job->job_id = context.job_id;
    job->model_id = context.model_id;
    job->prepared = std::move(context.request);
    job->prepared.request_id = context.job_id;
    job->callbacks = std::move(callbacks);
    return job;
}

TensorInferenceResponse PredictiveOrchestrator::executeBatch(
    const scheduler::PredictiveBatch& batch,
    IInferenceBackend& backend) const {
    if (batch.entries.empty()) {
        throw GenAIException(
            GenAIErrorCode::INVALID_REQUEST,
            "Cannot execute an empty predictive batch", 400);
    }
    if (batch.capacity == 0) {
        throw GenAIException(
            GenAIErrorCode::INVALID_REQUEST,
            "Predictive batch capacity must be greater than zero", 400);
    }

    const auto& first_job = batch.entries.front().job;
    if (!first_job) {
        throw std::runtime_error("Predictive batch contains a null job");
    }
    const auto& first_request = first_job->prepared;
    if (first_job->model_id != batch.model_id) {
        throw std::runtime_error("Predictive batch model mismatch");
    }
    size_t expected_offset = 0;
    for (const auto& entry : batch.entries) {
        if (!entry.job || entry.job->model_id != batch.model_id || entry.batch_count == 0 ||
            entry.batch_offset != expected_offset ||
            expected_offset > batch.capacity ||
            entry.batch_count > batch.capacity - expected_offset) {
            throw std::runtime_error("Invalid predictive batch layout");
        }
        expected_offset += entry.batch_count;
    }
    SegmentedTensorInferenceRequest request;
    request.model = batch.model_id;
    request.request_id = first_job->job_id;
    request.inputs.reserve(first_request.inputs.size());

    for (size_t input_index = 0; input_index < first_request.inputs.size(); ++input_index) {
        const auto& first = first_request.inputs[input_index];
        if (first.shape.empty() || first.shape.front() <= 0) {
            throw std::runtime_error("Predictive batch input has an invalid batch dimension");
        }
        const size_t first_batch = static_cast<size_t>(first.shape.front());
        if (first.data.size() % first_batch != 0) {
            throw std::runtime_error("Predictive batch input byte count is not divisible by its batch size");
        }
        const size_t bytes_per_sample = first.data.size() / first_batch;
        if (bytes_per_sample == 0 ||
            batch.capacity > std::numeric_limits<size_t>::max() / bytes_per_sample) {
            throw std::runtime_error("Predictive batch input byte count overflows");
        }

        SegmentedInputTensor input;
        input.name = first.name;
        input.shape = first.shape;
        input.shape.front() = static_cast<int64_t>(batch.capacity);
        input.dtype = first.dtype;
        input.byte_size = batch.capacity * bytes_per_sample;

        for (const auto& entry : batch.entries) {
            if (!entry.job || entry.job->prepared.inputs.size() != first_request.inputs.size() ||
                entry.batch_offset > batch.capacity ||
                entry.batch_count > batch.capacity - entry.batch_offset) {
                throw std::runtime_error("Invalid predictive batch entry");
            }
            const auto& source = entry.job->prepared.inputs[input_index];
            if (source.name != first.name || source.dtype != first.dtype ||
                source.shape.size() != first.shape.size() || source.shape.empty() ||
                source.shape.front() <= 0 ||
                static_cast<size_t>(source.shape.front()) != entry.batch_count ||
                source.data.size() % static_cast<size_t>(source.shape.front()) != 0 ||
                source.data.size() != entry.batch_count * bytes_per_sample) {
                throw std::runtime_error("Predictive batch inputs are incompatible");
            }
            for (size_t dimension = 1; dimension < first.shape.size(); ++dimension) {
                if (source.shape[dimension] != first.shape[dimension]) {
                    throw std::runtime_error("Predictive batch input shapes are incompatible");
                }
            }
            input.segments.push_back({source.data.data(), source.data.size()});
        }

        const ModelConfig* config =
            ModelConfigManager::getInstance().getModelConfig(batch.model_id);
        if (config && input_index < config->input_specs.size()) {
            input.padding_value = paddingValue(
                config->input_specs[input_index], input.dtype);
        }
        request.inputs.push_back(std::move(input));
    }

    bool all_outputs = false;
    std::vector<std::string> output_names;
    std::unordered_set<std::string> seen_outputs;
    for (const auto& entry : batch.entries) {
        if (!entry.job) throw std::runtime_error("Predictive batch contains a null job");
        if (entry.job->prepared.output_names.empty()) {
            all_outputs = true;
            break;
        }
        for (const auto& name : entry.job->prepared.output_names) {
            if (seen_outputs.insert(name).second) output_names.push_back(name);
        }
    }
    if (!all_outputs) request.output_names = output_names;

    if (expected_offset == 0) {
        throw std::runtime_error("Predictive batch contains no samples");
    }

    TensorInferenceResponse combined = backend.infer(request);
    combined.stats.backend_name = backend.name();
    return combined;
}
