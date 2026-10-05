// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// LiteRTPluginEntry — dlopen entry point for the "litert" predictive AI
// runtime plugin (qaiforge_plugin_litert.so).
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/backend/LiteRTBackend.h"
#include "qai_forge/plugin/PluginRegistry.h"

#include <memory>

extern "C" const char* qai_forge_plugin_abi_version() {
    return QAI_FORGE_PLUGIN_ABI_VERSION;
}

extern "C" void qai_forge_register_plugin(PluginRegistry& registry) {
    PluginRegistry::PredictiveEntry entry;
    entry.create_backend = []() -> std::unique_ptr<qai_forge::IInferenceBackend> {
        return std::make_unique<LiteRTBackend>();
    };
    registry.registerPredictive("litert", entry);
    // "tflite" is the same backend under an alternate runtime name used by
    // some model configs — register both.
    registry.registerPredictive("tflite", std::move(entry));
}
