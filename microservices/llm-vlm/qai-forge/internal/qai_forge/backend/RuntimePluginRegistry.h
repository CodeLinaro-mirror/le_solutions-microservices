// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/backend/IGenerativeBackend.h"
#include "qai_forge/backend/IInferenceBackend.h"
#include "qai_forge/orchestration/IGenerativeOrchestrator.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// RuntimePluginRegistry — central map of runtime name → backend/orchestrator
// factory functions, populated by plugin .so's at startup (via PluginLoader)
// or, in a statically-linked build, by each plugin's own translation unit
// registering directly against this same singleton.
//
// Two independent maps exist because IGenerativeBackend and IInferenceBackend
// are separate interfaces (generative vs predictive AI):
//   - Generative:  runtime → {create_backend, create_orchestrator}
//   - Predictive:  runtime → {create_backend}
//
// This is the seam BackendFactory delegates to instead of an #ifdef ladder —
// BackendFactory no longer needs to know which concrete backend types exist;
// it just asks the registry. Each plugin's qai_forge_register_plugin() entry
// point calls registerGenerative()/registerPredictive() for whichever
// runtime(s) it implements.
// ─────────────────────────────────────────────────────────────────────────────

class RuntimePluginRegistry {
public:
    struct GenerativeEntry {
        std::function<std::unique_ptr<IGenerativeBackend>()> create_backend;
        std::function<std::shared_ptr<IGenerativeOrchestrator>()> create_orchestrator;
    };

    struct PredictiveEntry {
        std::function<std::unique_ptr<IInferenceBackend>()> create_backend;
    };

    static RuntimePluginRegistry& instance();

    void registerGenerative(const std::string& runtime, GenerativeEntry entry);
    void registerPredictive(const std::string& runtime, PredictiveEntry entry);

    // Returns nullptr if the runtime is not registered.
    std::unique_ptr<IGenerativeBackend> createGenerativeBackend(
        const std::string& runtime) const;
    std::shared_ptr<IGenerativeOrchestrator> createGenerativeOrchestrator(
        const std::string& runtime) const;
    std::unique_ptr<IInferenceBackend> createPredictiveBackend(
        const std::string& runtime) const;

    std::vector<std::string> registeredGenerativeRuntimes() const;
    std::vector<std::string> registeredPredictiveRuntimes() const;

private:
    RuntimePluginRegistry() = default;
    RuntimePluginRegistry(const RuntimePluginRegistry&) = delete;
    RuntimePluginRegistry& operator=(const RuntimePluginRegistry&) = delete;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, GenerativeEntry> generative_;
    std::unordered_map<std::string, PredictiveEntry> predictive_;
};
