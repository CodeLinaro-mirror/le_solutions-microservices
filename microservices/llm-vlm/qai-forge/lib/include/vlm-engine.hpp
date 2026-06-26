// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// vlm-engine.hpp — Layer 4 VLM Engine
//
// Replaces vlm-service.hpp + vlm-interface.h + llm-buffer.h.
//
// VlmEngine wraps GeniePipeline and exposes a minimal C++ API:
//   - generate()    : run inference with optional images, stream tokens
//   - reset()       : reset pipeline state
//   - save_kv()     : save KV cache checkpoint
//   - restore_kv()  : restore KV cache checkpoint
//
// Layer 3 (genai-vlm-inference-worker) uses this class directly.
// No C interface, no opaque handles, no fixed-size char arrays.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once
#include "genai-engine.hpp"
#include <string>
#include <vector>
#include <memory>

class VlmEngine {
public:
    // Loads the model. Throws std::runtime_error on failure.
    VlmEngine(const std::string& model_id,
              const std::string& config_path,
              const std::string& sampler_config_path = "sampler.json");
    ~VlmEngine();

    // Non-copyable, non-movable (owns GenIE handles)
    VlmEngine(const VlmEngine&) = delete;
    VlmEngine& operator=(const VlmEngine&) = delete;
    VlmEngine(VlmEngine&&) = delete;
    VlmEngine& operator=(VlmEngine&&) = delete;

    // Runs inference synchronously. Invokes callback for each token.
    // images may be empty for text-only queries.
    // Blocks until generation is complete.
    // Throws std::runtime_error on inference failure.
    void generate(const std::string& prompt,
                  const std::vector<ImageBuffer>& images,
                  const GenerationConfig& config,
                  TokenCallback callback);

    void reset();
    void save_kv(const std::string& name);
    void restore_kv(const std::string& name);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
