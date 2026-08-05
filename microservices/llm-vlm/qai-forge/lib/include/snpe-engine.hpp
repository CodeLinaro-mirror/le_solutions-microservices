// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

// ─────────────────────────────────────────────────────────────────────────────
// SNPEEngine — Layer 4 wrapper around the SNPE C API
//
// Loads a SNPE DLC container model and runs inference using user-supplied
// buffers. The SNPE library (libSNPE.so) is loaded at runtime via dlopen —
// no direct linking required.
//
// Delegate options:
//   "none" — CPU only
//   "dsp"  — Hexagon DSP (HTP)
//   "gpu"  — Adreno GPU
//   "aip"  — Snapdragon AIX + HVX
//
// Usage:
//   SNPEEngine engine("/mnt/models/mobilenet/model.dlc", "dsp");
//   auto in_specs  = engine.inputSpecs();
//   auto out_specs = engine.outputSpecs();
//   engine.infer(input_ptrs, input_sizes, output_ptrs, output_sizes);
//
// See docs/unified-inference-service.md for the full design rationale.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Describes a single input or output tensor of the loaded model.
 */
struct SNPETensorSpec {
    std::string           name;   // tensor name from the model
    std::vector<size_t>   shape;  // tensor dimensions
    std::string           dtype;  // "float32", "uint8", "int8", etc.
    size_t                bytes;  // total byte size of the tensor
};

/**
 * SNPEEngine — wraps SNPE DLC inference with user-supplied buffers.
 * Non-copyable, non-movable (owns SNPE handles).
 */
class SNPEEngine {
public:
    /**
     * Load a SNPE DLC model and prepare for inference.
     *
     * @param model_file  Absolute path to the .dlc container file
     * @param delegate    Execution delegate: "none" | "dsp" | "gpu" | "aip"
     * @param output_tensors  Optional list of specific output tensor names.
     *                        If empty, all model outputs are returned.
     * @throws std::runtime_error on any initialization failure
     */
    SNPEEngine(const std::string&              model_file,
               const std::string&              delegate = "dsp",
               const std::vector<std::string>& output_tensors = {});

    ~SNPEEngine();

    // Non-copyable, non-movable (owns SNPE handles via dlopen)
    SNPEEngine(const SNPEEngine&) = delete;
    SNPEEngine& operator=(const SNPEEngine&) = delete;

    /**
     * Returns the input tensor specifications of the loaded model.
     */
    const std::vector<SNPETensorSpec>& inputSpecs() const;

    /**
     * Returns the output tensor specifications of the loaded model.
     */
    const std::vector<SNPETensorSpec>& outputSpecs() const;

    /**
     * Run inference synchronously using user-supplied buffers.
     *
     * @param input_ptrs   One raw byte buffer per input tensor
     * @param input_sizes  Byte size of each input buffer
     * @param output_ptrs  One pre-allocated raw byte buffer per output tensor
     * @param output_sizes Byte size of each output buffer
     * @throws std::runtime_error on inference failure
     */
    void infer(const std::vector<const uint8_t*>& input_ptrs,
               const std::vector<size_t>&          input_sizes,
               const std::vector<uint8_t*>&         output_ptrs,
               const std::vector<size_t>&           output_sizes);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
