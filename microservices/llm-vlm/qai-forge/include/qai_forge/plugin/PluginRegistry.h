// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

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
// Backend registration (registerGenerative/registerPredictive) remains
// internal-only for now — IGenerativeOrchestrator is not yet cleanly
// separable from internal scheduler state, so a public backend ABI needs
// its own follow-up design.
// ─────────────────────────────────────────────────────────────────────────────

class IGenerativeBackend;
class IInferenceBackend;
class IGenerativeOrchestrator;
class ModelAdapter;
class VisionPreprocessAdapter;

class PluginRegistry {
public:
    struct GenerativeEntry {
        std::function<std::unique_ptr<IGenerativeBackend>()> create_backend;
        std::function<std::shared_ptr<IGenerativeOrchestrator>()> create_orchestrator;
    };

    struct PredictiveEntry {
        std::function<std::unique_ptr<IInferenceBackend>()> create_backend;
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

    std::unique_ptr<IGenerativeBackend> createGenerativeBackend(
        const std::string& runtime) const;
    std::shared_ptr<IGenerativeOrchestrator> createGenerativeOrchestrator(
        const std::string& runtime) const;
    std::unique_ptr<IInferenceBackend> createPredictiveBackend(
        const std::string& runtime) const;

    std::vector<std::string> registeredGenerativeRuntimes() const;
    std::vector<std::string> registeredPredictiveRuntimes() const;

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
