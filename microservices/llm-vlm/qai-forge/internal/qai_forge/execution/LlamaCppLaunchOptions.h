// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/execution/ExecutionPlan.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace qai_forge {
namespace execution {

// Translates llama.cpp logical endpoint placement into worker INIT options.
// llama.cpp uses GGML_HEXAGON_DEVICES endpoint expressions rather than a
// provider-independent physical device ID.
class LlamaCppLaunchOptions {
public:
    // Automatic backend-managed plans return std::nullopt. Explicit plans
    // return the selector exactly as supplied by the plan.
    static std::optional<std::string> hexagonDevices(
        const ExecutionPlan& plan);

    // Automatic plans return an empty object.
    static nlohmann::ordered_json toInitOptions(
        const ExecutionPlan& plan);
};

}  // namespace execution
}  // namespace qai_forge
