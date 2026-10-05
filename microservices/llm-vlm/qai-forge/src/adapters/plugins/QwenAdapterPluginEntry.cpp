// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// QwenAdapterPluginEntry — dlopen entry point for the Qwen model-adapter
// plugin (qaiforge_plugin_adapter_qwen.so).
//
// Registers Qwen3Adapter + Qwen25Adapter (model adapters) and
// QwenVisionPreprocessAdapter (vision adapter) against PluginRegistry.
// Qwen3 is registered at a higher priority than Qwen2.5 so a model_id that
// could ambiguously match both ("qwen3" takes precedence over the broader
// "qwen2.5"/"qwen2-5"/"qwen2_5" substrings) resolves correctly — mirroring
// the original ModelAdapterFactory if/else ordering, now expressed via
// explicit priority instead of code order.
// ─────────────────────────────────────────────────────────────────────────────

#include "qai_forge/adapters/Qwen25Adapter.h"
#include "qai_forge/adapters/Qwen3Adapter.h"
#include "qai_forge/adapters/QwenVisionPreprocessAdapter.h"
#include "qai_forge/plugin/PluginRegistry.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>

namespace {

std::string toLower(const std::string& s) {
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return lower;
}

bool matchesQwen3(const std::string& model_id) {
    const std::string lower_id = toLower(model_id);
    return lower_id.find("qwen3") != std::string::npos ||
           lower_id.find("qwen-3") != std::string::npos ||
           lower_id.find("qwq") != std::string::npos;
}

bool matchesQwen25(const std::string& model_id) {
    const std::string lower_id = toLower(model_id);
    return lower_id.find("qwen2.5") != std::string::npos ||
           lower_id.find("qwen2-5") != std::string::npos ||
           lower_id.find("qwen2_5") != std::string::npos;
}

bool matchesQwenVision(const std::string& model_id) {
    return toLower(model_id).find("qwen") != std::string::npos;
}

} // namespace

extern "C" const char* qai_forge_plugin_abi_version() {
    return QAI_FORGE_PLUGIN_ABI_VERSION;
}

extern "C" void qai_forge_register_plugin(PluginRegistry& registry) {
    PluginRegistry::ModelAdapterEntry qwen3_entry;
    qwen3_entry.matches = matchesQwen3;
    qwen3_entry.create = []() -> std::shared_ptr<ModelAdapter> {
        return std::make_shared<Qwen3Adapter>();
    };
    qwen3_entry.priority = 20;
    qwen3_entry.name = "Qwen3Adapter";
    registry.registerModelAdapter(std::move(qwen3_entry));

    PluginRegistry::ModelAdapterEntry qwen25_entry;
    qwen25_entry.matches = matchesQwen25;
    qwen25_entry.create = []() -> std::shared_ptr<ModelAdapter> {
        return std::make_shared<Qwen25Adapter>();
    };
    qwen25_entry.priority = 10;
    qwen25_entry.name = "Qwen25Adapter";
    registry.registerModelAdapter(std::move(qwen25_entry));

    PluginRegistry::VisionAdapterEntry qwen_vision_entry;
    qwen_vision_entry.matches = matchesQwenVision;
    qwen_vision_entry.create = []() -> std::shared_ptr<VisionPreprocessAdapter> {
        return std::make_shared<QwenVisionPreprocessAdapter>();
    };
    qwen_vision_entry.priority = 10;
    qwen_vision_entry.name = "QwenVisionPreprocessAdapter";
    registry.registerVisionAdapter(std::move(qwen_vision_entry));
}
