// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/plugin/PluginRegistry.h"

#include "qai_forge/adapters/ModelAdapter.h"
#include "qai_forge/adapters/VisionPreprocessAdapter.h"
#include "qai_forge/adapters/DefaultAdapter.h"
#include "qai_forge/adapters/DefaultVisionPreprocessAdapter.h"
#include "qai_forge/backend/BackendAdapter.h"
#include "qai_forge/backend/IGenerativeBackend.h"
#include "qai_forge/backend/IInferenceBackend.h"
#include "qai_forge/backend/PredictiveBackendAdapter.h"
#include "qai_forge/orchestration/IGenerativeOrchestrator.h"
#include "qai_forge/orchestration/OrchestratorAdapter.h"
#include "qai_forge/utils/Logger.h"

#include <algorithm>

PluginRegistry::PluginRegistry()
    : default_model_adapter_(std::make_shared<DefaultAdapter>()),
      default_vision_adapter_(std::make_shared<DefaultVisionPreprocessAdapter>()) {}

PluginRegistry& PluginRegistry::instance() {
    static PluginRegistry registry;
    return registry;
}

// ── Backend / orchestrator ──────────────────────────────────────────────────

void PluginRegistry::registerGenerative(const std::string& runtime,
                                         GenerativeEntry entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    generative_[runtime] = std::move(entry);
}

void PluginRegistry::registerPredictive(const std::string& runtime,
                                         PredictiveEntry entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    predictive_[runtime] = std::move(entry);
}

std::unique_ptr<BackendAdapter> PluginRegistry::createGenerativeBackend(
    const std::string& runtime) const {
    std::unique_ptr<qai_forge::IGenerativeBackend> public_backend;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = generative_.find(runtime);
        if (it == generative_.end() || !it->second.create_backend) {
            return nullptr;
        }
        public_backend = it->second.create_backend();
    }
    if (!public_backend) {
        return nullptr;
    }
    return std::make_unique<BackendAdapter>(std::move(public_backend));
}

std::shared_ptr<OrchestratorAdapter> PluginRegistry::createGenerativeOrchestrator(
    const std::string& runtime) const {
    std::shared_ptr<qai_forge::IGenerativeOrchestrator> public_orchestrator;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = generative_.find(runtime);
        if (it == generative_.end() || !it->second.create_orchestrator) {
            return nullptr;
        }
        public_orchestrator = it->second.create_orchestrator();
    }
    if (!public_orchestrator) {
        return nullptr;
    }
    // Wrap the public, out-of-tree-safe qai_forge::IGenerativeOrchestrator
    // in the internal OrchestratorAdapter so scheduler code (ModelRuntime/
    // WarmModelPool/PostTurnWorker) continues to see only the private
    // IGenerativeOrchestrator type, exactly as before this change.
    return std::make_shared<OrchestratorAdapter>(std::move(public_orchestrator));
}

std::unique_ptr<PredictiveBackendAdapter> PluginRegistry::createPredictiveBackend(
    const std::string& runtime) const {
    std::unique_ptr<qai_forge::IInferenceBackend> public_backend;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = predictive_.find(runtime);
        if (it == predictive_.end() || !it->second.create_backend) {
            return nullptr;
        }
        public_backend = it->second.create_backend();
    }
    if (!public_backend) {
        return nullptr;
    }
    return std::make_unique<PredictiveBackendAdapter>(std::move(public_backend));
}

std::vector<std::string> PluginRegistry::registeredGenerativeRuntimes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> runtimes;
    runtimes.reserve(generative_.size());
    for (const auto& [runtime, entry] : generative_) {
        runtimes.push_back(runtime);
    }
    return runtimes;
}

std::vector<std::string> PluginRegistry::registeredPredictiveRuntimes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> runtimes;
    runtimes.reserve(predictive_.size());
    for (const auto& [runtime, entry] : predictive_) {
        runtimes.push_back(runtime);
    }
    return runtimes;
}

void PluginRegistry::validateRegistrations() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [runtime, entry] : generative_) {
        const bool has_backend = static_cast<bool>(entry.create_backend);
        const bool has_orchestrator = static_cast<bool>(entry.create_orchestrator);
        if (has_backend && !has_orchestrator) {
            LOG_WARN("[PluginRegistry] Generative runtime '" << runtime
                     << "' registered a backend factory but no orchestrator "
                        "factory — requests for this runtime will fail at "
                        "first use (see BackendFactory::createGenerativeOrchestrator). "
                        "Check the plugin's qai_forge_register_plugin() entry point.");
        } else if (!has_backend && has_orchestrator) {
            LOG_WARN("[PluginRegistry] Generative runtime '" << runtime
                     << "' registered an orchestrator factory but no backend "
                        "factory — requests for this runtime will fail at "
                        "first use (see BackendFactory::createGenerativeBackend). "
                        "Check the plugin's qai_forge_register_plugin() entry point.");
        } else if (!has_backend && !has_orchestrator) {
            LOG_WARN("[PluginRegistry] Generative runtime '" << runtime
                     << "' is registered with neither a backend nor an "
                        "orchestrator factory — this entry is a no-op. Check "
                        "the plugin's qai_forge_register_plugin() entry point.");
        }
    }

    for (const auto& [runtime, entry] : predictive_) {
        if (!entry.create_backend) {
            LOG_WARN("[PluginRegistry] Predictive runtime '" << runtime
                     << "' is registered with no backend factory — this "
                        "entry is a no-op. Check the plugin's "
                        "qai_forge_register_plugin() entry point.");
        }
    }
}

// ── Model / vision adapters ─────────────────────────────────────────────────

void PluginRegistry::registerModelAdapter(ModelAdapterEntry entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    model_adapters_.push_back(std::move(entry));
    model_adapter_instances_.resize(model_adapters_.size());
}

void PluginRegistry::registerVisionAdapter(VisionAdapterEntry entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    vision_adapters_.push_back(std::move(entry));
    vision_adapter_instances_.resize(vision_adapters_.size());
}

const ModelAdapter& PluginRegistry::resolveModelAdapter(
    const std::string& model_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Build an index order sorted by priority (descending), stable within
    // equal priority (registration order preserved).
    std::vector<std::size_t> order(model_adapters_.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [this](std::size_t a, std::size_t b) {
        return model_adapters_[a].priority > model_adapters_[b].priority;
    });

    for (std::size_t idx : order) {
        const auto& entry = model_adapters_[idx];
        if (entry.matches && entry.matches(model_id)) {
            if (!model_adapter_instances_[idx]) {
                model_adapter_instances_[idx] = entry.create ? entry.create() : nullptr;
            }
            if (model_adapter_instances_[idx]) {
                return *model_adapter_instances_[idx];
            }
        }
    }
    return *default_model_adapter_;
}

const VisionPreprocessAdapter& PluginRegistry::resolveVisionAdapter(
    const std::string& model_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::size_t> order(vision_adapters_.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [this](std::size_t a, std::size_t b) {
        return vision_adapters_[a].priority > vision_adapters_[b].priority;
    });

    for (std::size_t idx : order) {
        const auto& entry = vision_adapters_[idx];
        if (entry.matches && entry.matches(model_id)) {
            if (!vision_adapter_instances_[idx]) {
                vision_adapter_instances_[idx] = entry.create ? entry.create() : nullptr;
            }
            if (vision_adapter_instances_[idx]) {
                return *vision_adapter_instances_[idx];
            }
        }
    }
    return *default_vision_adapter_;
}

std::vector<std::string> PluginRegistry::registeredModelAdapters() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(model_adapters_.size());
    for (const auto& entry : model_adapters_) {
        names.push_back(entry.name);
    }
    return names;
}

std::vector<std::string> PluginRegistry::registeredVisionAdapters() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(vision_adapters_.size());
    for (const auto& entry : vision_adapters_) {
        names.push_back(entry.name);
    }
    return names;
}
