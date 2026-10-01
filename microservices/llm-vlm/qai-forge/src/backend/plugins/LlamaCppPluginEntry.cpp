// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// LlamaCppPluginEntry — dlopen entry point for the "llamacpp" generative
// runtime plugin (qaiforge_plugin_llamacpp.so).
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/backend/LlamaCppBackend.h"
#include "qai_forge/plugin/PluginRegistry.h"
#include "qai_forge/orchestration/LlamaCppOrchestrator.h"

#include <memory>

extern "C" void qai_forge_register_plugin(PluginRegistry& registry) {
    PluginRegistry::GenerativeEntry entry;
    entry.create_backend = []() -> std::unique_ptr<IGenerativeBackend> {
        return std::make_unique<qai_forge::LlamaCppBackend>();
    };
    entry.create_orchestrator = []() -> std::shared_ptr<IGenerativeOrchestrator> {
        return std::make_shared<qai_forge::LlamaCppOrchestrator>();
    };
    registry.registerGenerative("llamacpp", std::move(entry));
}
