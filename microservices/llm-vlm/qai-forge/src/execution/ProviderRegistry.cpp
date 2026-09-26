// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/execution/ProviderRegistry.h"

namespace qai_forge {
namespace execution {

void ProviderRegistry::registerProvider(const std::string& provider_id,
                                         ProviderCapabilities capabilities) {
    providers_[provider_id] = capabilities;
}

std::optional<ProviderCapabilities> ProviderRegistry::capabilitiesOf(
    const std::string& provider_id) const {
    auto it = providers_.find(provider_id);
    if (it == providers_.end()) return std::nullopt;
    return it->second;
}

ProviderRegistry ProviderRegistry::createDefault() {
    ProviderRegistry registry;

    // QNN: direct platform enumeration, exact binding.
    {
        ProviderCapabilities caps;
        caps.enumerates_physical_targets = true;
        caps.supports_exact_binding = true;
        caps.supports_runtime_preference = false;
        caps.supports_fallback = false;
        caps.supports_multi_target = false;
        caps.reports_effective_target = true;
        caps.requires_process_isolation = true;
        registry.registerProvider("qnn", caps);
    }

    // GenIE/QAIRT: configured device_id, component-level, not yet exact.
    {
        ProviderCapabilities caps;
        caps.enumerates_physical_targets = false;
        caps.supports_exact_binding = false;
        caps.supports_runtime_preference = false;
        caps.supports_fallback = false;
        caps.supports_multi_target = true;
        caps.reports_effective_target = false;
        caps.requires_process_isolation = true;
        registry.registerProvider("genie", caps);
    }

    // llama.cpp (Hexagon): configured registry, exact endpoints.
    {
        ProviderCapabilities caps;
        caps.enumerates_physical_targets = false;
        caps.supports_exact_binding = true;
        caps.supports_runtime_preference = false;
        caps.supports_fallback = false;
        caps.supports_multi_target = true;
        caps.reports_effective_target = true;
        caps.requires_process_isolation = true;
        registry.registerProvider("llamacpp", caps);
    }

    // SNPE: ordered runtime preference with CPU fallback.
    {
        ProviderCapabilities caps;
        caps.enumerates_physical_targets = false;
        caps.supports_exact_binding = false;
        caps.supports_runtime_preference = true;
        caps.supports_fallback = true;
        caps.supports_multi_target = false;
        caps.reports_effective_target = false;
        caps.requires_process_isolation = true;
        registry.registerProvider("snpe", caps);
    }

    // LiteRT (predictive): accelerator class set, backend-managed physical.
    {
        ProviderCapabilities caps;
        caps.enumerates_physical_targets = false;
        caps.supports_exact_binding = false;
        caps.supports_runtime_preference = false;
        caps.supports_fallback = true;
        caps.supports_multi_target = false;
        caps.reports_effective_target = false;
        caps.requires_process_isolation = true;
        registry.registerProvider("litert", caps);
    }

    // LiteRT-LM: component backend classes, backend-managed physical.
    {
        ProviderCapabilities caps;
        caps.enumerates_physical_targets = false;
        caps.supports_exact_binding = false;
        caps.supports_runtime_preference = false;
        caps.supports_fallback = true;
        caps.supports_multi_target = false;
        caps.reports_effective_target = false;
        caps.requires_process_isolation = true;
        registry.registerProvider("litert_lm", caps);
    }

    return registry;
}

}  // namespace execution
}  // namespace qai_forge
