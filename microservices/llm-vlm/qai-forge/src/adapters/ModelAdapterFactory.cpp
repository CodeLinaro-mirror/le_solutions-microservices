// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/adapters/ModelAdapterFactory.h"
#include "qai_forge/plugin/PluginRegistry.h"

// ─────────────────────────────────────────────────────────────────────────────
// ModelAdapterFactory::getAdapter
//
// Thin forwarding shim over PluginRegistry::resolveModelAdapter(). All
// resolution logic (priority-ordered matching, built-in Default fallback,
// Qwen/Gemma plugin registration) now lives in PluginRegistry — this
// factory no longer has compile-time knowledge of any concrete adapter
// type, matching BackendFactory's existing delegation pattern.
// ─────────────────────────────────────────────────────────────────────────────
const ModelAdapter& ModelAdapterFactory::getAdapter(const std::string& model_id) {
    return PluginRegistry::instance().resolveModelAdapter(model_id);
}
