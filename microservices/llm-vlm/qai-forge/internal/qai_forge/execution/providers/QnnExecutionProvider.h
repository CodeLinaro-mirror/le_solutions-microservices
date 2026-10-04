// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// QnnExecutionProvider - IExecutionProvider for direct QNN.
//
// Enumerates physical targets exactly: one candidatePlans() entry per HTP
// node whose native_ids map contains a "qnn" key. Relocated from
// PlacementAdmissionGateway::tryAcquireAutomatic()'s QNN branch and
// qnnExactPlan().

#pragma once

#include "qai_forge/execution/IExecutionProvider.h"

namespace qai_forge {
namespace execution {

class QnnExecutionProvider : public IExecutionProvider {
public:
    std::string id() const override { return "qnn"; }

    ProviderCapabilities capabilities() const override;

    ExecutionPlan defaultPlan(
        const std::string& model_id,
        const resources::TopologyRegistry& topology) const override;

    std::vector<ExecutionPlan> candidatePlans(
        const std::string& model_id,
        const resources::TopologyRegistry& topology) const override;

    nlohmann::ordered_json toInitOptions(
        const ExecutionPlan& plan) const override;

    // Builds the exact plan for one native QNN device_id. Exposed for
    // PlacementAdmissionGateway's existing tryAcquireQnnDevice() path.
    static ExecutionPlan exactPlan(
        uint32_t qnn_device_id,
        const resources::TopologyRegistry& topology);
};

}  // namespace execution
}  // namespace qai_forge
