// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/backend/RuntimePluginRegistry.h"

RuntimePluginRegistry& RuntimePluginRegistry::instance() {
    static RuntimePluginRegistry registry;
    return registry;
}

void RuntimePluginRegistry::registerGenerative(const std::string& runtime,
                                               GenerativeEntry entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    generative_[runtime] = std::move(entry);
}

void RuntimePluginRegistry::registerPredictive(const std::string& runtime,
                                               PredictiveEntry entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    predictive_[runtime] = std::move(entry);
}

std::unique_ptr<IGenerativeBackend> RuntimePluginRegistry::createGenerativeBackend(
    const std::string& runtime) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = generative_.find(runtime);
    if (it == generative_.end() || !it->second.create_backend) {
        return nullptr;
    }
    return it->second.create_backend();
}

std::shared_ptr<IGenerativeOrchestrator> RuntimePluginRegistry::createGenerativeOrchestrator(
    const std::string& runtime) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = generative_.find(runtime);
    if (it == generative_.end() || !it->second.create_orchestrator) {
        return nullptr;
    }
    return it->second.create_orchestrator();
}

std::unique_ptr<IInferenceBackend> RuntimePluginRegistry::createPredictiveBackend(
    const std::string& runtime) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = predictive_.find(runtime);
    if (it == predictive_.end() || !it->second.create_backend) {
        return nullptr;
    }
    return it->second.create_backend();
}

std::vector<std::string> RuntimePluginRegistry::registeredGenerativeRuntimes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> runtimes;
    runtimes.reserve(generative_.size());
    for (const auto& [runtime, entry] : generative_) {
        runtimes.push_back(runtime);
    }
    return runtimes;
}

std::vector<std::string> RuntimePluginRegistry::registeredPredictiveRuntimes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> runtimes;
    runtimes.reserve(predictive_.size());
    for (const auto& [runtime, entry] : predictive_) {
        runtimes.push_back(runtime);
    }
    return runtimes;
}
