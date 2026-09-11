// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/scheduler/PredictiveJob.h"

class IInferenceBackend;

class PredictiveOrchestrator {
public:
    /** @brief Validate and prepare a predictive inference job. */
    scheduler::PredictiveJobPtr createJob(
        scheduler::PredictiveJobContext context,
        scheduler::PredictiveCallbacks callbacks) const;

    /**  Execute a scheduler-created batch and return the combined response. */
    TensorInferenceResponse executeBatch(
        const scheduler::PredictiveBatch& batch,
        IInferenceBackend& backend) const;
};
