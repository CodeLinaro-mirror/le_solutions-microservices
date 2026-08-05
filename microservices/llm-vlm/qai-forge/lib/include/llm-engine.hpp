// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// llm-engine.hpp — Layer 4 LLM Engine
//
// Replaces llm-service.hpp + llm-interface.h + llm-buffer.h.
//
// LlmEngine wraps GenieDialog and exposes a minimal C++ API:
//   - generate()    : run inference, stream tokens via TokenCallback
//   - reset()       : reset dialog KV cache
//   - save_kv()     : save KV cache checkpoint
//   - restore_kv()  : restore KV cache checkpoint
//
// Layer 3 (genai-inference-worker) uses this class directly.
// No C interface, no opaque handles, no fixed-size char arrays.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once
#include "genai-engine.hpp"
#include <string>
#include <memory>

class LlmEngine {
public:
    // Loads the model. Throws std::runtime_error on failure.
    LlmEngine(const std::string& model_id,
              const std::string& config_path,
              const std::string& sampler_config_path = "sampler.json");
    ~LlmEngine();

    // Non-copyable, non-movable (owns GenIE handles)
    LlmEngine(const LlmEngine&) = delete;
    LlmEngine& operator=(const LlmEngine&) = delete;
    LlmEngine(LlmEngine&&) = delete;
    LlmEngine& operator=(LlmEngine&&) = delete;

    // Runs inference synchronously. Invokes callback for each token.
    // Blocks until generation is complete.
    // Throws std::runtime_error on inference failure.
    void generate(const std::string& prompt,
                  const GenerationConfig& config,
                  TokenCallback callback);

    void reset();
    void save_kv(const std::string& name);
    void restore_kv(const std::string& name);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
