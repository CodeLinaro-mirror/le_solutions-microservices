// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/adapters/ModelAdapter.h"
#include <string>
#include <memory>

// ─────────────────────────────────────────────────────────────────────────────
// ModelAdapterFactory — Resolves the correct ModelAdapter for a given model_id
//
// Called at the start of the ChatOrchestratorImpl pipeline (Step 1).
// Returns a reference to a stateless, singleton adapter instance.
//
// Resolution logic (case-insensitive substring match on model_id):
//   "qwen2.5" or "qwen2-5" → Qwen25Adapter
//   "qwen3"                → Qwen3Adapter
//   "qwq"                  → Qwen3Adapter (same tool/thinking format)
//   anything else          → DefaultAdapter
//
// Adapters are stateless singletons — safe to share across threads.
// ─────────────────────────────────────────────────────────────────────────────
class ModelAdapterFactory {
public:
    /**
     * Get the appropriate adapter for the given model_id.
     * Returns a reference to a stateless singleton adapter.
     * Never returns nullptr.
     */
    static const ModelAdapter& getAdapter(const std::string& model_id);

private:
    ModelAdapterFactory() = delete;
};
