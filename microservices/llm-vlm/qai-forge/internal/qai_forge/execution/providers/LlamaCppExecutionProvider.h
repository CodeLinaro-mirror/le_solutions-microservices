// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// LlamaCppExecutionProvider - IExecutionProvider for llama.cpp Hexagon.
// No physical-target enumeration in the current placement path (endpoint
// selection is configured via GGML_HEXAGON_DEVICES at the worker level) -
// uses the default single-candidate candidatePlans().

#pragma once

#include "qai_forge/execution/IExecutionProvider.h"

namespace qai_forge {
namespace execution {

class LlamaCppExecutionProvider : public IExecutionProvider {
public:
    std::string id() const override { return "llamacpp"; }

    ProviderCapabilities capabilities() const override;

    ExecutionPlan defaultPlan(
        const std::string& model_id,
        const resources::TopologyRegistry& topology) const override;

    nlohmann::ordered_json toInitOptions(
        const ExecutionPlan& plan) const override;
};

}  // namespace execution
}  // namespace qai_forge
