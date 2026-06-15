// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/backend/IGenerativeBackend.h"
#include "qai_forge/worker/InferenceWorkerManager.h"
#include <memory>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// LiteRTLMBackend — IGenerativeBackend implementation for LiteRT-LM
//
// Wraps InferenceWorkerManager to manage the litert-lm-inference-worker
// subprocess. Uses the same JSON-Lines-over-socketpair IPC protocol as
// GenIEBackend, but the worker binary uses LiteRT-LM's LlmInference API
// instead of GenieDialog.
//
// LiteRT-LM is built on top of LiteRT (same version as LiteRT_builder stage).
// The @litert Bazel repository is overridden via --override_repository so
// LiteRT-LM reuses our already-compiled LiteRT artifacts.
//
// metadata.json fields used:
//   "runtime":     "litert_lm"
//   "model_file":  "model.litertlm"  (or .task / .tflite with LLM metadata)
//
// Environment variable overrides:
//   LITERT_LM_WORKER_BINARY — path to litert-lm-inference-worker binary
// ─────────────────────────────────────────────────────────────────────────────

class LiteRTLMBackend : public IGenerativeBackend {
public:
    static LiteRTLMBackend& getInstance();

    std::string name() const override { return "LiteRT-LM"; }

    BackendCapabilities capabilities() const override;

    void ensureWorkerRunning(const std::string& model_id,
                             const std::string& config_file,
                             const std::string& sampler_config) override;

    void generate(
        const std::string& event_id,
        const std::string& prompt,
        bool               streaming,
        int                max_tokens,
        float              temperature,
        float              top_p,
        int                top_k,
        float              presence_penalty,
        float              frequency_penalty,
        bool               use_reasoning,
        std::function<void(const IPCTokenEvent&)>  on_token,
        std::function<void(const IPCDoneEvent&)>   on_done,
        std::function<void(const IPCErrorEvent&)>  on_error) override;

    void generateVlm(
        const std::string&              event_id,
        const std::string&              prompt,
        const std::vector<std::string>& image_paths,
        bool                            streaming,
        int                             max_tokens,
        float                           temperature,
        float                           top_p,
        int                             top_k,
        float                           presence_penalty,
        float                           frequency_penalty,
        std::function<void(const IPCTokenEvent&)>  on_token,
        std::function<void(const IPCDoneEvent&)>   on_done,
        std::function<void(const IPCErrorEvent&)>  on_error) override;

    void onContextCompacted() override;

    void saveKv(const std::string& name) override;
    void restoreKv(const std::string& name) override;
    void resetKv() override;

    void terminateWorker(bool force) override;
    bool isHealthy() const override;

private:
    LiteRTLMBackend();
    LiteRTLMBackend(const LiteRTLMBackend&)            = delete;
    LiteRTLMBackend& operator=(const LiteRTLMBackend&) = delete;

    std::unique_ptr<InferenceWorkerManager> worker_;
    std::string current_model_id_;
};
