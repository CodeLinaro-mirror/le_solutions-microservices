// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#ifdef QAI_FORGE_BUILD_LLAMACPP

#include "qai_forge/worker/InferenceWorkerManager.h"
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// LlamaCppWorkerManager — llamacpp-inference-worker process manager
//
// Extends InferenceWorkerManager exactly like LiteRTLMWorkerManager, but
// needs no ensureWorkerRunning() override: the llamacpp worker reads its
// chat template directly from the GGUF file (llama_model_chat_template())
// at load time, so there is no separate METADATA handshake to intercept —
// the base class's standard READY → INIT → READY sequence is sufficient.
// ─────────────────────────────────────────────────────────────────────────────

class LlamaCppWorkerManager : public InferenceWorkerManager {
public:
    /**
     * Constructor.
     * Passes "llamacpp" as the process_type to InferenceWorkerManager so
     * that startWorker() selects the correct binary (LLAMACPP_WORKER_BINARY
     * or /usr/local/bin/llamacpp-inference-worker) and socket FD env var.
     */
    LlamaCppWorkerManager();
};

#endif // QAI_FORGE_BUILD_LLAMACPP
