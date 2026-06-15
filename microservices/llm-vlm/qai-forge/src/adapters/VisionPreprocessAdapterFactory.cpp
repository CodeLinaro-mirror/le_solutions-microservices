// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/adapters/VisionPreprocessAdapterFactory.h"
#include "qai_forge/adapters/DefaultVisionPreprocessAdapter.h"
#include "qai_forge/adapters/QwenVisionPreprocessAdapter.h"
#include "qai_forge/utils/Logger.h"
#include <algorithm>
#include <cctype>

// ─────────────────────────────────────────────────────────────────────────────
// VisionPreprocessAdapterFactory::getAdapter
//
// Resolves the correct vision preprocessing adapter for a given model_id using
// case-insensitive substring matching. Returns a reference to a stateless
// singleton adapter — safe to share across threads.
//
// Detection rules (evaluated in order):
//   "qwen"  → QwenVisionPreprocessAdapter  (Qwen2-VL, Qwen2.5-VL, Qwen3-VL)
//   (other) → DefaultVisionPreprocessAdapter
// ─────────────────────────────────────────────────────────────────────────────
const VisionPreprocessAdapter& VisionPreprocessAdapterFactory::getAdapter(
    const std::string& model_id) {

    // Static singleton instances — stateless, safe to share across threads
    static const QwenVisionPreprocessAdapter    qwen_adapter;
    static const DefaultVisionPreprocessAdapter default_adapter;

    // Normalize to lowercase for case-insensitive matching
    std::string lower_id = model_id;
    std::transform(lower_id.begin(), lower_id.end(), lower_id.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // ── Qwen VLM family (Qwen2-VL, Qwen2.5-VL, Qwen3-VL) ────────────────────
    // All Qwen vision-language models use the same block-major patch ordering.
    if (lower_id.find("qwen") != std::string::npos) {
        LOG_DEBUG("[VisionPreprocessAdapterFactory] Resolved QwenVisionPreprocessAdapter for: "
                  << model_id);
        return qwen_adapter;
    }

    // ── Default fallback ───────────────────────────────────────────────────────
    LOG_DEBUG("[VisionPreprocessAdapterFactory] Using DefaultVisionPreprocessAdapter for: "
              << model_id);
    return default_adapter;
}
