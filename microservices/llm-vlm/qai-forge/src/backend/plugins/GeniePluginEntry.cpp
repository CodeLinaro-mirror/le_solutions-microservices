// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// GeniePluginEntry — dlopen entry point for the "genie" generative runtime
// plugin (qaiforge_plugin_genie.so).
//
// Registers GenIEBackend + GenieOrchestrator against whichever
// RuntimePluginRegistry is passed in — either the core qai_forge.so's
// singleton (when dlopen'd by PluginLoader) or, in a build where this file
// happens to be linked directly into a consumer, the same process-wide
// singleton (RuntimePluginRegistry::instance() is a single definition
// regardless of which .so first touches it, as long as this plugin links
// against the same qai_forge.so).
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/backend/GenIEBackend.h"
#include "qai_forge/backend/RuntimePluginRegistry.h"
#include "qai_forge/orchestration/GenieOrchestrator.h"

#include <memory>

extern "C" void qai_forge_register_plugin(RuntimePluginRegistry& registry) {
    RuntimePluginRegistry::GenerativeEntry entry;
    entry.create_backend = []() -> std::unique_ptr<IGenerativeBackend> {
        return std::make_unique<GenIEBackend>();
    };
    entry.create_orchestrator = []() -> std::shared_ptr<IGenerativeOrchestrator> {
        return std::make_shared<GenieOrchestrator>();
    };
    registry.registerGenerative("genie", std::move(entry));
}
