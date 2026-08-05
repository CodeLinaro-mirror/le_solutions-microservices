// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/backend/IInferenceBackend.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include <memory>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// QNNBackend — IInferenceBackend implementation for QNN conventional AI
//
// Wraps ConventionalWorkerManager to manage the qnn-inference-worker subprocess.
// Supports QNN context binary (.bin) models compiled for HTP/GPU/CPU backends.
//
// metadata.json fields used:
//   "runtime":     "qnn"
//   "model_file":  "model_htp.bin"
//   "backend_lib": "/usr/lib/libQnnHtp.so"
//   "sys_lib":     "/usr/lib/libQnnSystem.so"
// ─────────────────────────────────────────────────────────────────────────────

class ConventionalWorkerManager;

class QNNBackend : public IInferenceBackend {
public:
    static QNNBackend& getInstance();

    std::string name() const override { return "QNN"; }

    void initialize(const std::string& model_id,
                    const std::string& model_file) override;

    TensorInferenceResponse infer(const TensorInferenceRequest& request) override;

    bool isHealthy() const override;
    void shutdown() override;

private:
    QNNBackend();
    QNNBackend(const QNNBackend&) = delete;
    QNNBackend& operator=(const QNNBackend&) = delete;

    std::unique_ptr<ConventionalWorkerManager> worker_;
    std::string current_model_id_;
};
