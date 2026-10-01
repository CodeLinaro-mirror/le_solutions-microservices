// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/adapters/VisionPreprocessAdapterFactory.h"
#include "qai_forge/plugin/PluginRegistry.h"

// ─────────────────────────────────────────────────────────────────────────────
// VisionPreprocessAdapterFactory::getAdapter
//
// Thin forwarding shim over PluginRegistry::resolveVisionAdapter(). All
// resolution logic (priority-ordered matching, built-in Default fallback,
// Qwen vision-adapter plugin registration) now lives in PluginRegistry.
// ─────────────────────────────────────────────────────────────────────────────
const VisionPreprocessAdapter& VisionPreprocessAdapterFactory::getAdapter(
    const std::string& model_id) {
    return PluginRegistry::instance().resolveVisionAdapter(model_id);
}
