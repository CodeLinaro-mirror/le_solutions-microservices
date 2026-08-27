// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/InternalDTOs.h"
#include "qai_forge/dto/TensorDTOs.h"

#include <functional>
#include <memory>
#include <string>

namespace scheduler {

struct PredictiveCallbacks {
    std::function<void(const TensorInferenceResponse&)> on_complete;
    std::function<void(const GenAIException&)> on_error;
};

struct PredictiveJobContext {
    std::string job_id;
    std::string model_id;
    TensorInferenceRequest request;
};

struct PredictiveJob {
    std::string job_id;
    std::string model_id;
    TensorInferenceRequest prepared;
    PredictiveCallbacks callbacks;

    PredictiveJob() = default;
    ~PredictiveJob() = default;

    PredictiveJob(const PredictiveJob&) = delete;
    PredictiveJob& operator=(const PredictiveJob&) = delete;
    PredictiveJob(PredictiveJob&&) = delete;
    PredictiveJob& operator=(PredictiveJob&&) = delete;
};

using PredictiveJobPtr = std::shared_ptr<PredictiveJob>;

} // namespace scheduler
