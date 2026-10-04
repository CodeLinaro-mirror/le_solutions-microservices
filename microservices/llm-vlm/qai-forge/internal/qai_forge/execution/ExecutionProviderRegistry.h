// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ExecutionProviderRegistry - owns one IExecutionProvider instance per
// provider id. Replaces the old capability-only qai_forge::execution::
// ProviderRegistry (static ProviderCapabilities lookup, no behavior).
//
// capabilitiesOf() is kept for existing callers (ExecutionPlanResolver)
// that only need capability data, forwarding to provider->capabilities().

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "qai_forge/execution/IExecutionProvider.h"

namespace qai_forge {
namespace execution {

class ExecutionProviderRegistry {
public:
    // Registers all six built-in providers: qnn, genie, llamacpp, snpe,
    // litert, litert_lm.
    static ExecutionProviderRegistry createDefault();

    void registerProvider(std::shared_ptr<IExecutionProvider> provider);

    std::shared_ptr<IExecutionProvider> get(
        const std::string& provider_id) const;

    std::optional<ProviderCapabilities> capabilitiesOf(
        const std::string& provider_id) const;

    bool isRegistered(const std::string& provider_id) const {
        return providers_.find(provider_id) != providers_.end();
    }

private:
    std::unordered_map<std::string, std::shared_ptr<IExecutionProvider>>
        providers_;
};

}  // namespace execution
}  // namespace qai_forge
