// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/backend/PredictiveSharedMemory.h"
#include "qai_forge/dto/TensorDTOs.h"
#include <string>

namespace qai_forge {

// ─────────────────────────────────────────────────────────────────────────────
// qai_forge::IInferenceBackend — public, out-of-tree ("Level B") Predictive
// AI backend ABI.
//
// This is the contract a third party implements to plug a new predictive
// (tensor-in/tensor-out) inference backend into qai-forge without access to
// qai-forge's source tree or internal scheduler plumbing.
//
// NOTE on history: this interface was promoted from an internal,
// global-namespace ::IInferenceBackend (internal/qai_forge/backend/
// IInferenceBackend.h, now deleted) — same contract, minus the
// placement-aware initialize(RuntimeLaunchRequest) overload, which remains
// an internal-only concern (see internal/qai_forge/backend/
// IPlacementAwarePredictiveBackend.h — the device-placement subsystem is a
// separate, not-yet-public ABI). internal/qai_forge/backend/
// PredictiveBackendAdapter.h wraps any registered qai_forge::IInferenceBackend
// and adds back placement-aware initialization via dynamic_cast to that
// internal mixin, with a graceful fallback to initialize(model_id, {}) for
// backends (including out-of-tree third-party ones) that don't implement it.
//
// Implemented internally (see internal/qai_forge/backend/) by:
//   LiteRTBackend — wraps TFLite C++ API (TfLiteInterpreter)
//   QNNBackend    — wraps QNN SDK C++ API
//   SNPEBackend   — wraps SNPE SDK C++ API
//
// Design invariants:
//   - PredictiveOrchestrator ONLY calls methods on this interface.
//   - Tensor data is always raw bytes — no base64, no JSON arrays.
//   - The backend runs in a subprocess (same fault isolation as GenIEBackend).
//   - infer() is blocking — returns when all outputs are ready.
//
// Contrast with qai_forge::IGenerativeBackend:
//   - qai_forge::IGenerativeBackend: text in → streaming tokens out (stateful, sessions)
//   - IInferenceBackend:  tensors in → tensors out (stateless, no sessions)
// ─────────────────────────────────────────────────────────────────────────────

class IInferenceBackend {
public:
    virtual ~IInferenceBackend() = default;

    // Human-readable name for logging ("LiteRT", "QNN", "SNPE")
    virtual std::string name() const = 0;

    /**
     * Initialize the backend with a model.
     * Called once when the model is first loaded.
     * Throws std::runtime_error on failure.
     *
     * @param model_id    Model identifier
     * @param model_file  Absolute path to the model file (.tflite, .bin, etc.)
     */
    virtual void initialize(const std::string& model_id,
                            const std::string& model_file) = 0;

    /**
     * Run inference from non-owning tensor segments. Implementations write the
     * segments and padding directly into backend-owned shared memory.
     */
    virtual TensorInferenceResponse infer(
        const SegmentedTensorInferenceRequest& request) = 0;

    /**
     * Write request input bytes directly into backend-owned shared memory and
     * return a compact worker IPC request containing only data references.
     * Throws std::runtime_error if the input layout exceeds the shared-memory
     * input capacity.
     */
    virtual PredictiveExecuteRequest prepareWorkerRequest(
        const SegmentedTensorInferenceRequest& request) = 0;

    /**
     * Returns true if the backend worker subprocess is alive and responsive.
     */
    virtual bool isHealthy() const = 0;

    /**
     * Graceful shutdown — sends SHUTDOWN command to worker, waits for exit.
     */
    virtual void shutdown() = 0;
};

} // namespace qai_forge
