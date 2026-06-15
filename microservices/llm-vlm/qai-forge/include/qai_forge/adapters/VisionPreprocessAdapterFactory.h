// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/adapters/VisionPreprocessAdapter.h"
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// VisionPreprocessAdapterFactory — Resolves the correct VLM preprocessing
// adapter from a model_id string.
//
// Uses case-insensitive substring matching on the model_id, following the
// same pattern as ModelAdapterFactory. Returns a const reference to a
// stateless singleton adapter — safe to share across threads.
//
// Detection rules (evaluated in order):
//   "qwen"  → QwenVisionPreprocessAdapter  (Qwen2-VL, Qwen2.5-VL, Qwen3-VL)
//   (other) → DefaultVisionPreprocessAdapter
//
// To add support for a new VLM model family:
//   1. Create a new adapter class derived from VisionPreprocessAdapter.
//   2. Add a detection rule in VisionPreprocessAdapterFactory.cpp.
//   3. Add the new .cpp file to QAI_FORGE_SOURCES in CMakeLists.txt.
// ─────────────────────────────────────────────────────────────────────────────
class VisionPreprocessAdapterFactory {
public:
    /**
     * Resolve the preprocessing adapter for the given model_id.
     *
     * @param model_id  Model identifier string (e.g. "qwen2.5-vl-7b-instruct").
     *                  Case-insensitive substring matching is used.
     * @return          Const reference to a stateless singleton adapter.
     *                  The reference is valid for the lifetime of the process.
     */
    static const VisionPreprocessAdapter& getAdapter(const std::string& model_id);
};
