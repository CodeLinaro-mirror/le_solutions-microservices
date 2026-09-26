// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/execution/ExecutionPlan.h"

#include <nlohmann/json.hpp>

namespace qai_forge {
namespace execution {

// Translates a LiteRT accelerator-class execution plan into provider-native
// worker INIT options. No LiteRT SDK dependency.
//
// Native bit values match LiteRT's public HwAcceleratorSet: CPU=1, GPU=2,
// NPU=4.
class LiteRtLaunchOptions {
public:
    // Returns the LiteRT hardware-accelerator bitmask. Automatic/backend-
    // managed plans return the legacy default NPU|CPU mask. Throws for
    // malformed, conflicting, or non-LiteRT plans.
    static int hwAccelerators(const ExecutionPlan& plan);

    // Automatic plans return an empty object so the worker preserves its
    // legacy defaults.
    static nlohmann::ordered_json toInitOptions(const ExecutionPlan& plan);
};

}  // namespace execution
}  // namespace qai_forge
