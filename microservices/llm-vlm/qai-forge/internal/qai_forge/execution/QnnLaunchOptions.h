// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/execution/ExecutionPlan.h"

#include <cstdint>
#include <optional>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace qai_forge {
namespace execution {

// Converts an exact QNN component target into the provider-native INIT
// options consumed by the QNN worker.
class QnnLaunchOptions {
public:
    // Returns std::nullopt for an automatic plan. Throws
    // std::invalid_argument for malformed or non-QNN plans.
    static std::optional<uint32_t> selectedDeviceId(
        const ExecutionPlan& plan);

    // An automatic plan returns an empty JSON object.
    static nlohmann::ordered_json toInitOptions(
        const ExecutionPlan& plan);
};

}  // namespace execution
}  // namespace qai_forge
