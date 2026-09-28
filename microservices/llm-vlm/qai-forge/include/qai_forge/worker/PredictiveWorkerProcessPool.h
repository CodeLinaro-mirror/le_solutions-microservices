// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/worker/PredictiveWorkerChannel.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

// ─────────────────────────────────────────────────────────────────────────────
// PredictiveWorkerProcessPool — process-registry keyed by (binary, process_type)
//
// Provides one PredictiveWorkerChannel instance per distinct worker binary
// path, shared across all backend instances of that runtime family. This is
// the "one subprocess per inference runtime type, N models multiplexed
// inside it" policy: QNNBackend, SNPEBackend, and LiteRTBackend each look up
// (lazily creating) a shared channel here instead of each owning a private,
// per-model worker-manager instance (the older one-process-per-model design).
//
// Keyed by worker_binary rather than a fixed enum so tests / alternate
// deployments can point at different binaries without code changes, while
// still guaranteeing exactly one process per distinct binary path.
// ─────────────────────────────────────────────────────────────────────────────
class PredictiveWorkerProcessPool {
public:
    static PredictiveWorkerProcessPool& getInstance();

    /**
     * Get (lazily creating) the shared channel for a given worker binary.
     * The returned reference is stable for the process lifetime of the
     * caller — channels are never destroyed except at pool teardown.
     */
    PredictiveWorkerChannel& getChannel(const std::string& worker_binary,
                                        const std::string& process_type);

    /** Test/shutdown helper — terminates and drops all channels. */
    void shutdownAll() noexcept;

private:
    PredictiveWorkerProcessPool() = default;

    std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<PredictiveWorkerChannel>> channels_;
};
