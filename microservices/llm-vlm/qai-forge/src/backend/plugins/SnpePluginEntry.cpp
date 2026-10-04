// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// SnpePluginEntry — dlopen entry point for the "snpe" predictive AI runtime
// plugin (qaiforge_plugin_snpe.so).
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/backend/SNPEBackend.h"
#include "qai_forge/plugin/PluginRegistry.h"

#include <memory>

extern "C" void qai_forge_register_plugin(PluginRegistry& registry) {
    PluginRegistry::PredictiveEntry entry;
    entry.create_backend = []() -> std::unique_ptr<qai_forge::IInferenceBackend> {
        return std::make_unique<SNPEBackend>();
    };
    registry.registerPredictive("snpe", entry);
    // "qnn_dlc" is the same backend under an alternate runtime name used by
    // some model configs — register both.
    registry.registerPredictive("qnn_dlc", std::move(entry));
}
