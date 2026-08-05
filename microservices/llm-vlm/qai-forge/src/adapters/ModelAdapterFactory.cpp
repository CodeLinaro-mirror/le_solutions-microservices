// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/adapters/ModelAdapterFactory.h"
#include "qai_forge/adapters/Qwen25Adapter.h"
#include "qai_forge/adapters/Qwen3Adapter.h"
#include "qai_forge/adapters/DefaultAdapter.h"
#include "qai_forge/utils/Logger.h"
#include <algorithm>
#include <cctype>

// ─────────────────────────────────────────────────────────────────────────────
// ModelAdapterFactory::getAdapter
//
// Resolves the correct adapter for a given model_id using case-insensitive
// substring matching. Returns a reference to a stateless singleton adapter.
// ─────────────────────────────────────────────────────────────────────────────
const ModelAdapter& ModelAdapterFactory::getAdapter(const std::string& model_id) {
    // Static singleton instances — stateless, safe to share across threads
    static const Qwen25Adapter  qwen25_adapter;
    static const Qwen3Adapter   qwen3_adapter;
    static const DefaultAdapter default_adapter;

    // Normalize to lowercase for case-insensitive matching
    std::string lower_id = model_id;
    std::transform(lower_id.begin(), lower_id.end(), lower_id.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // ── Qwen 3 family (check before Qwen 2.5 to avoid false match) ────────────
    if (lower_id.find("qwen3") != std::string::npos ||
        lower_id.find("qwen-3") != std::string::npos ||
        lower_id.find("qwq") != std::string::npos) {
        LOG_DEBUG("[ModelAdapterFactory] Resolved Qwen3Adapter for: " << model_id);
        return qwen3_adapter;
    }

    // ── Qwen 2.5 family ────────────────────────────────────────────────────────
    if (lower_id.find("qwen2.5") != std::string::npos ||
        lower_id.find("qwen2-5") != std::string::npos ||
        lower_id.find("qwen2_5") != std::string::npos) {
        LOG_DEBUG("[ModelAdapterFactory] Resolved Qwen25Adapter for: " << model_id);
        return qwen25_adapter;
    }

    // ── Default fallback ───────────────────────────────────────────────────────
    LOG_DEBUG("[ModelAdapterFactory] Using DefaultAdapter for: " << model_id);
    return default_adapter;
}
