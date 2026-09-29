// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// -----------------------------------------------------------------------------
// ProviderRegistry - Static provider capability descriptors
// -----------------------------------------------------------------------------

#pragma once

#include <optional>
#include <string>
#include <unordered_map>

#include "qai_forge/execution/ExecutionPlan.h"

namespace qai_forge {
namespace execution {

class ProviderRegistry {
public:
    // Returns a registry pre-populated with capability descriptors for:
    // "qnn", "genie", "llamacpp", "snpe", "litert", "litert_lm".
    static ProviderRegistry createDefault();

    void registerProvider(const std::string& provider_id,
                           ProviderCapabilities capabilities);

    std::optional<ProviderCapabilities> capabilitiesOf(
        const std::string& provider_id) const;

    bool isRegistered(const std::string& provider_id) const {
        return providers_.find(provider_id) != providers_.end();
    }

private:
    std::unordered_map<std::string, ProviderCapabilities> providers_;
};

}  // namespace execution
}  // namespace qai_forge
