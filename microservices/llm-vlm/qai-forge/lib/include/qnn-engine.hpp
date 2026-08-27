// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

// ─────────────────────────────────────────────────────────────────────────────
// QNNEngine — Layer 4 wrapper around the QNN C API
//
// Loads a pre-compiled QNN context binary (.bin) and runs inference.
// The QNN backend library (libQnnHtp.so, libQnnCpu.so, etc.) and the
// QNN system library (libQnnSystem.so) are loaded at runtime via dlopen —
// no direct linking required.
//
// Only the cached context binary path is supported (pre-compiled offline).
// The uncached path (QnnModel_composeGraphs from a .so) is not needed for
// production deployment.
//
// Usage:
//   QNNEngine engine("/mnt/models/mobilenet/model_htp.bin",
//                    "/usr/lib/libQnnHtp.so",
//                    "/usr/lib/libQnnSystem.so");
//   auto in_specs  = engine.inputSpecs();
//   auto out_specs = engine.outputSpecs();
//   engine.infer(inputs, outputs);
//
// See docs/unified-inference-service.md for the full design rationale.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Describes a single input or output tensor of the loaded model.
 */
struct QNNTensorSpec {
    std::string              name;   // tensor name from the model
    std::vector<uint32_t>    shape;  // tensor dimensions
    int                      dtype;  // Qnn_DataType_t value
    std::string              dtype_str; // KFServing v2 string, e.g. "FP32"/"UINT8"
    size_t                   bytes;  // total tensor size in bytes (shape * elem width)
    float                    scale;  // quantization scale (0.0 if not quantized)
    int32_t                  offset; // quantization offset (0 if not quantized)
};

/**
 * QNNEngine — wraps QNN context binary inference.
 * Non-copyable, non-movable (owns QNN handles).
 */
class QNNEngine {
public:
    /**
     * Load a QNN context binary and prepare for inference.
     *
     * @param model_file   Absolute path to the .bin context binary
     * @param backend_lib  Absolute path to the QNN backend library
     *                     (e.g. /usr/lib/libQnnHtp.so)
     * @param sys_lib      Absolute path to the QNN system library
     *                     (e.g. /usr/lib/libQnnSystem.so)
     * @throws std::runtime_error on any initialization failure
     */
    QNNEngine(const std::string& model_file,
              const std::string& backend_lib,
              const std::string& sys_lib);

    ~QNNEngine();

    // Non-copyable, non-movable (owns QNN handles via dlopen)
    QNNEngine(const QNNEngine&) = delete;
    QNNEngine& operator=(const QNNEngine&) = delete;

    /**
     * Returns the input tensor specifications of the loaded model.
     * Call after construction to discover input shapes and types.
     */
    const std::vector<QNNTensorSpec>& inputSpecs() const;

    /**
     * Returns the output tensor specifications of the loaded model.
     */
    const std::vector<QNNTensorSpec>& outputSpecs() const;

    /**
     * Run inference synchronously.
     *
     * @param inputs   One raw byte buffer per input tensor, in the order
     *                 returned by inputSpecs(). Caller owns the memory.
     * @param outputs  One raw byte buffer per output tensor, pre-allocated
     *                 to the size indicated by outputSpecs(). Caller owns.
     * @throws std::runtime_error on inference failure
     */
    void infer(const std::vector<const uint8_t*>& input_ptrs,
               const std::vector<size_t>&         input_sizes,
               const std::vector<uint8_t*>&        output_ptrs,
               const std::vector<size_t>&          output_sizes);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
