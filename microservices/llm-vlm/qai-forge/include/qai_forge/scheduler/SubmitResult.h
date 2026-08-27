// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <string>

namespace scheduler {

enum class SubmitStatus {
    QUEUED,
    REJECTED_MODEL_NOT_FOUND,
    REJECTED_QUEUE_FULL,
    REJECTED_ADMISSION_TIMEOUT,
    REJECTED_SHUTTING_DOWN,
    REJECTED_PREVIOUS_RESPONSE_NOT_FOUND,
    REJECTED_TOOL_RESPONSE_TIMEOUT,
};

struct SubmitResult {
    SubmitStatus status = SubmitStatus::QUEUED;
    std::string job_id;
    std::string message;

    bool accepted() const {
        return status == SubmitStatus::QUEUED;
    }
};

} // namespace scheduler
