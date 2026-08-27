// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/backend/IGenerativeBackend.h"
#include "qai_forge/scheduler/GenerativeJob.h"

class IGenerativeOrchestrator {
public:
    virtual ~IGenerativeOrchestrator() = default;

    virtual scheduler::GenerativeJobPtr createJob(
        scheduler::GenerativeJobContext context,
        scheduler::GenerativeCallbacks callbacks) const = 0;

    virtual StandardResponse execute(
        scheduler::GenerativeJob& job,
        IGenerativeBackend& backend) const = 0;
};
