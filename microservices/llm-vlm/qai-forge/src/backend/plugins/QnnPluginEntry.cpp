// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// QnnPluginEntry — dlopen entry point for the "qnn" predictive AI runtime
// plugin (qaiforge_plugin_qnn.so).
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/backend/QNNBackend.h"
#include "qai_forge/plugin/PluginRegistry.h"

#include <memory>

extern "C" const char* qai_forge_plugin_abi_version() {
    return QAI_FORGE_PLUGIN_ABI_VERSION;
}

extern "C" void qai_forge_register_plugin(PluginRegistry& registry) {
    PluginRegistry::PredictiveEntry entry;
    entry.create_backend = []() -> std::unique_ptr<qai_forge::IInferenceBackend> {
        return std::make_unique<QNNBackend>();
    };
    registry.registerPredictive("qnn", entry);
    // "qnn_context_binary" is the same backend under an alternate runtime
    // name used by some model configs — register both.
    registry.registerPredictive("qnn_context_binary", std::move(entry));
}
