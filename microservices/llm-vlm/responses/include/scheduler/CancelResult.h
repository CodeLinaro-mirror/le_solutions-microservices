// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <string>

namespace scheduler {

enum class CancelStatus {
    QUEUED_CANCELLED,
    RUNNING_CANCELLED,
    NOT_FOUND
};

enum class RunningCancelMode {
    SOFT,
    HARD
};

struct CancelResult {
    CancelStatus status = CancelStatus::NOT_FOUND;
    std::string job_id;
    std::string message;

    bool cancelled() const {
        return status == CancelStatus::QUEUED_CANCELLED ||
               status == CancelStatus::RUNNING_CANCELLED;
    }
};

} // namespace scheduler
