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

    /** @brief Execute and normalize a prepared predictive inference job. */
    TensorInferenceResponse execute(
        scheduler::PredictiveJob& job,
        IInferenceBackend& backend) const;
};
