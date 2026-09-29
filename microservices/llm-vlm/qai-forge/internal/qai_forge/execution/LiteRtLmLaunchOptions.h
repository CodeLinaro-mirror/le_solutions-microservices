// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/execution/ExecutionPlan.h"

#include <nlohmann/json.hpp>

namespace qai_forge {
namespace execution {

// Translates LiteRT-LM backend-class plans into provider-native INIT data.
// LiteRT-LM exposes backend classes and session affinity rather than a
// physical HTP selector, so this adapter never produces a physical device
// ID.
class LiteRtLmLaunchOptions {
public:
    // Automatic plans with no backend selector return an empty object.
    // A selector uses the form "backend=npu", "backend=cpu", or
    // "backend=gpu".
    static nlohmann::ordered_json componentBackends(
        const ExecutionPlan& plan);

    // Backend-managed plans without explicit selectors return an empty
    // object.
    static nlohmann::ordered_json toInitOptions(
        const ExecutionPlan& plan);
};

}  // namespace execution
}  // namespace qai_forge
