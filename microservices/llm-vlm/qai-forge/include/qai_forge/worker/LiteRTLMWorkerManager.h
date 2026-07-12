// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/worker/InferenceWorkerManager.h"
#include <string>
#include <atomic>

// ─────────────────────────────────────────────────────────────────────────────
// LiteRTLMWorkerManager — Phase 2: Extended worker manager for LiteRT-LM
//
// Extends InferenceWorkerManager with LiteRT-LM-specific startup handling:
//
//   1. Overrides ensureWorkerRunning() to intercept the METADATA message that
//      the litert-lm-inference-worker sends after loading the model.
//      The METADATA message carries the Jinja template, model type, context
//      length, and tool call delimiters — information the orchestrator needs
//      to render prompts and parse tool calls without loading model weights
//      in the main process.
//
//   2. Exposes getMetadata() so LiteRTLMBackend can pass the metadata to
//      LiteRTLMOrchestrator at construction time.
//
// Extended startup sequence (vs. standard InferenceWorkerManager):
//
//   Standard:
//     Worker → READY
//     Parent → INIT
//     Worker → READY
//
//   LiteRT-LM (extended):
//     Worker → READY
//     Parent → INIT
//     Worker → METADATA  ← intercepted here, stored in metadata_
//     Worker → READY
//
// The METADATA interception is done by overriding ensureWorkerRunning() and
// hooking into the post-INIT READY wait. The base class sendCommandAndWaitReady()
// drains stale messages, so we need to intercept before that drain loop.
//
// Thread safety: same as InferenceWorkerManager — all operations are
// serialised through the inherited mutex_.
// ─────────────────────────────────────────────────────────────────────────────

struct LiteRTLMMetadata {
    std::string jinja_template;
    std::string model_type;
    int         max_context_length      = 4096;
    std::string tool_call_delimiter;
    std::string tool_response_delimiter;
    bool        received                = false;
};

class LiteRTLMWorkerManager : public InferenceWorkerManager {
public:
    /**
     * Constructor.
     * Passes "litert-lm" as the process_type to InferenceWorkerManager so
     * that startWorker() selects the correct binary (LITERT_LM_WORKER_BINARY
     * or /usr/local/bin/litert-lm-inference-worker) and socket FD env var.
     */
    LiteRTLMWorkerManager();

    /**
     * Ensure the litert-lm-inference-worker is running with the given model.
     *
     * Overrides the base class to intercept the METADATA message that the
     * worker sends after loading the model. The metadata is stored in
     * metadata_ and exposed via getMetadata().
     *
     * @param model_id     Model identifier
     * @param config_file  Absolute path to the .litertlm model file
     * @param sampler_config  Unused for LiteRT-LM (pass empty string)
     */
    void ensureWorkerRunning(const std::string& model_id,
                             const std::string& config_file,
                             const std::string& sampler_config) override;

    /**
     * Return the metadata received from the worker after model load.
     *
     * Valid only after ensureWorkerRunning() has been called successfully.
     * Returns a default-constructed LiteRTLMMetadata (received=false) if
     * the worker has not yet sent METADATA.
     */
    const LiteRTLMMetadata& getMetadata() const { return metadata_; }

private:
    LiteRTLMMetadata metadata_;
};
