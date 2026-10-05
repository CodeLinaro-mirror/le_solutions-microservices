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
// Plugin ABI version — see docs/PLUGIN_DEVELOPMENT.md.
//
// Every plugin .so must export a matching
//   extern "C" const char* qai_forge_plugin_abi_version();
// returning exactly this string. PluginLoader checks this before calling
// qai_forge_register_plugin() and skips (with a logged warning) any plugin
// whose reported version does not match, instead of risking a layout
// mismatch against this process's PluginRegistry/GenerativeEntry/
// PredictiveEntry/ModelAdapterEntry/VisionAdapterEntry struct definitions.
//
// Bump this value whenever a change to PluginRegistry (or any struct it
// exposes to plugins — GenerativeEntry, PredictiveEntry, ModelAdapterEntry,
// VisionAdapterEntry) could break binary compatibility with a plugin .so
// built against an older header.
// ─────────────────────────────────────────────────────────────────────────────
inline constexpr const char* QAI_FORGE_PLUGIN_ABI_VERSION = "1.0.0";

// ─────────────────────────────────────────────────────────────────────────────
// PluginRegistry — central registry for qai-forge pluggable capabilities.
//
// Renamed/relocated from the old backend-only RuntimePluginRegistry — this
// is now the
// shared registration point for both runtime backends and model/vision
// adapters, not a backend-only concept.
//
// Model/vision adapter registration is a public, out-of-tree ("Level B")
// extension point: third parties can implement ModelAdapter /
// VisionPreprocessAdapter, export qai_forge_register_plugin(PluginRegistry&),
// build their own .so, and drop it into the plugin directory — no access to
// qai-forge's source or build system required.
//
// Backend/orchestrator registration (registerGenerative/registerPredictive)
// is likewise callable from any plugin's qai_forge_register_plugin() entry
// point using only public headers (qai_forge::IGenerativeBackend,
// qai_forge::IGenerativeOrchestrator, qai_forge::IInferenceBackend) — see
// docs/PLUGIN_DEVELOPMENT.md for the full contract and a worked example.
// Device-placement-aware loading (IPlacementAwareGenerativeBackend /
// IPlacementAwarePredictiveBackend) remains an internal-only mixin on top
// of this — see the "Device placement" section of that doc for why.
// ─────────────────────────────────────────────────────────────────────────────

class BackendAdapter;
class PredictiveBackendAdapter;
class OrchestratorAdapter;
class ModelAdapter;
class VisionPreprocessAdapter;

class PluginRegistry {
public:
    struct GenerativeEntry {
        // Public ABI: plugin entry points construct and return a
        // qai_forge::IGenerativeBackend — PluginRegistry wraps it in an
        // internal BackendAdapter before returning it from
        // createGenerativeBackend() below, so scheduler code continues to
        // see only the internal BackendAdapter type.
        std::function<std::unique_ptr<qai_forge::IGenerativeBackend>()>
            create_backend;
        // Public ABI: plugin entry points construct and return a
        // qai_forge::IGenerativeOrchestrator — PluginRegistry wraps it in
        // an internal OrchestratorAdapter before returning it from
        // createGenerativeOrchestrator() below, so scheduler code
        // continues to see only the private IGenerativeOrchestrator type.
        std::function<std::shared_ptr<qai_forge::IGenerativeOrchestrator>()>
            create_orchestrator;
    };

    struct PredictiveEntry {
        // Public ABI: plugin entry points construct and return a
        // qai_forge::IInferenceBackend — PluginRegistry wraps it in an
        // internal PredictiveBackendAdapter before returning it from
        // createPredictiveBackend() below.
        std::function<std::unique_ptr<qai_forge::IInferenceBackend>()>
            create_backend;
    };

    // `matches` tests a model_id; `priority` breaks ties when multiple
    // entries match (higher checked first). `create` returns a shared,
    // stateless adapter instance — cached per entry after first creation.
    struct ModelAdapterEntry {
        std::function<bool(const std::string& model_id)> matches;
        std::function<std::shared_ptr<ModelAdapter>()> create;
        int priority = 0;
        std::string name;
    };

    struct VisionAdapterEntry {
        std::function<bool(const std::string& model_id)> matches;
        std::function<std::shared_ptr<VisionPreprocessAdapter>()> create;
        int priority = 0;
        std::string name;
    };

    static PluginRegistry& instance();

    // ── Backend / orchestrator (internal use) ──────────────────────────────
    void registerGenerative(const std::string& runtime, GenerativeEntry entry);
    void registerPredictive(const std::string& runtime, PredictiveEntry entry);

    std::unique_ptr<BackendAdapter> createGenerativeBackend(
        const std::string& runtime) const;
    std::shared_ptr<OrchestratorAdapter> createGenerativeOrchestrator(
        const std::string& runtime) const;
    std::unique_ptr<PredictiveBackendAdapter> createPredictiveBackend(
        const std::string& runtime) const;

    std::vector<std::string> registeredGenerativeRuntimes() const;
    std::vector<std::string> registeredPredictiveRuntimes() const;

    // Logs a warning (via Logger) for every registered generative runtime
    // that has a backend factory but no orchestrator factory, or vice
    // versa — a registration mistake that would otherwise only surface as
    // a late, confusing failure the first time that runtime is actually
    // requested (see BackendFactory::createRuntimePair()). Intended to be
    // called once at startup, immediately after PluginLoader::loadAll(),
    // so misconfigured plugins are caught at process-startup time instead
    // of at first-request time. Does not throw — this is a diagnostic
    // check, not an admission gate.
    void validateRegistrations() const;

    // ── Model / vision adapters (public ABI) ───────────────────────────────
    void registerModelAdapter(ModelAdapterEntry entry);
    void registerVisionAdapter(VisionAdapterEntry entry);

    // Highest-priority matching entry wins; falls back to the built-in
    // Default adapter (never null, never removable).
    const ModelAdapter& resolveModelAdapter(const std::string& model_id) const;
    const VisionPreprocessAdapter& resolveVisionAdapter(const std::string& model_id) const;

    std::vector<std::string> registeredModelAdapters() const;
    std::vector<std::string> registeredVisionAdapters() const;

private:
    PluginRegistry();
    PluginRegistry(const PluginRegistry&) = delete;
    PluginRegistry& operator=(const PluginRegistry&) = delete;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, GenerativeEntry> generative_;
    std::unordered_map<std::string, PredictiveEntry> predictive_;

    std::vector<ModelAdapterEntry> model_adapters_;
    std::vector<VisionAdapterEntry> vision_adapters_;

    mutable std::vector<std::shared_ptr<ModelAdapter>> model_adapter_instances_;
    mutable std::vector<std::shared_ptr<VisionPreprocessAdapter>> vision_adapter_instances_;

    std::shared_ptr<ModelAdapter> default_model_adapter_;
    std::shared_ptr<VisionPreprocessAdapter> default_vision_adapter_;
};
