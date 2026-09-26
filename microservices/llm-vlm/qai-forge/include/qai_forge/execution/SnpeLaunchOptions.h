// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/execution/ExecutionPlan.h"

#include <nlohmann/json.hpp>

namespace qai_forge {
namespace execution {

// Translates an SNPE runtime-preference execution plan into provider-native
// worker INIT options.
class SnpeLaunchOptions {
public:
    // Returns an empty string for an automatic/backend-managed plan. Throws
    // for malformed or non-SNPE plans.
    static std::string runtimeOrder(const ExecutionPlan& plan);

    // Automatic plans return an empty object.
    static nlohmann::ordered_json toInitOptions(const ExecutionPlan& plan);
};

}  // namespace execution
}  // namespace qai_forge
