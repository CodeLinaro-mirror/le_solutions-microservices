// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// GemmaAdapterPluginEntry — dlopen entry point for the Gemma model-adapter
// plugin (qaiforge_plugin_adapter_gemma.so).
//
// Registers GemmaAdapter against PluginRegistry's model-adapter extension
// point.
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/adapters/GemmaAdapter.h"
#include "qai_forge/plugin/PluginRegistry.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>

namespace {

bool matchesGemma(const std::string& model_id) {
    std::string lower_id = model_id;
    std::transform(lower_id.begin(), lower_id.end(), lower_id.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return lower_id.find("gemma") != std::string::npos;
}

} // namespace

extern "C" const char* qai_forge_plugin_abi_version() {
    return QAI_FORGE_PLUGIN_ABI_VERSION;
}

extern "C" void qai_forge_register_plugin(PluginRegistry& registry) {
    PluginRegistry::ModelAdapterEntry entry;
    entry.matches = matchesGemma;
    entry.create = []() -> std::shared_ptr<ModelAdapter> {
        return std::make_shared<GemmaAdapter>();
    };
    entry.priority = 10;
    entry.name = "GemmaAdapter";
    registry.registerModelAdapter(std::move(entry));
}
