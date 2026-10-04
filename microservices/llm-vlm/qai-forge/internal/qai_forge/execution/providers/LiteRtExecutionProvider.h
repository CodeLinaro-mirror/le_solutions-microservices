// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// LiteRtExecutionProvider - IExecutionProvider for LiteRT (predictive).
// Accelerator-class-set selection (NPU|CPU); no physical-HTP enumeration.

#pragma once

#include "qai_forge/execution/IExecutionProvider.h"

namespace qai_forge {
namespace execution {

class LiteRtExecutionProvider : public IExecutionProvider {
public:
    std::string id() const override { return "litert"; }

    ProviderCapabilities capabilities() const override;

    ExecutionPlan defaultPlan(
        const std::string& model_id,
        const resources::TopologyRegistry& topology) const override;

    nlohmann::ordered_json toInitOptions(
        const ExecutionPlan& plan) const override;
};

}  // namespace execution
}  // namespace qai_forge
