// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/execution/ExecutionProviderRegistry.h"

#include "qai_forge/execution/providers/GenieExecutionProvider.h"
#include "qai_forge/execution/providers/LiteRtExecutionProvider.h"
#include "qai_forge/execution/providers/LiteRtLmExecutionProvider.h"
#include "qai_forge/execution/providers/LlamaCppExecutionProvider.h"
#include "qai_forge/execution/providers/QnnExecutionProvider.h"
#include "qai_forge/execution/providers/SnpeExecutionProvider.h"

namespace qai_forge {
namespace execution {

ExecutionProviderRegistry ExecutionProviderRegistry::createDefault() {
    ExecutionProviderRegistry registry;
    registry.registerProvider(std::make_shared<QnnExecutionProvider>());
    registry.registerProvider(std::make_shared<GenieExecutionProvider>());
    registry.registerProvider(std::make_shared<LlamaCppExecutionProvider>());
    registry.registerProvider(std::make_shared<SnpeExecutionProvider>());
    registry.registerProvider(std::make_shared<LiteRtExecutionProvider>());
    registry.registerProvider(std::make_shared<LiteRtLmExecutionProvider>());
    return registry;
}

void ExecutionProviderRegistry::registerProvider(
    std::shared_ptr<IExecutionProvider> provider) {
    if (!provider) return;
    providers_[provider->id()] = std::move(provider);
}

std::shared_ptr<IExecutionProvider> ExecutionProviderRegistry::get(
    const std::string& provider_id) const {
    auto it = providers_.find(provider_id);
    return it == providers_.end() ? nullptr : it->second;
}

std::optional<ProviderCapabilities> ExecutionProviderRegistry::capabilitiesOf(
    const std::string& provider_id) const {
    auto it = providers_.find(provider_id);
    if (it == providers_.end()) return std::nullopt;
    return it->second->capabilities();
}

}  // namespace execution
}  // namespace qai_forge
