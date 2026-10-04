// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// GenieExecutionProvider - IExecutionProvider for GenIE/QAIRT.
//
// Configured (not enumerated) physical selection: one candidatePlans()
// entry per HTP node whose native_ids map contains a "genie" key, each
// patching devices[0].device_id. Relocated from
// PlacementAdmissionGateway::tryAcquireAutomatic()'s GenIE branch and
// geniePreferredPlan().

#pragma once

#include "qai_forge/execution/IExecutionProvider.h"

namespace qai_forge {
namespace execution {

class GenieExecutionProvider : public IExecutionProvider {
public:
    std::string id() const override { return "genie"; }

    ProviderCapabilities capabilities() const override;

    ExecutionPlan defaultPlan(
        const std::string& model_id,
        const resources::TopologyRegistry& topology) const override;

    std::vector<ExecutionPlan> candidatePlans(
        const std::string& model_id,
        const resources::TopologyRegistry& topology) const override;

    nlohmann::ordered_json toInitOptions(
        const ExecutionPlan& plan) const override;

    // Builds the preferred plan for one native GenIE device_id.
    static ExecutionPlan preferredPlan(
        uint32_t genie_device_id,
        const resources::TopologyRegistry& topology);
};

}  // namespace execution
}  // namespace qai_forge
