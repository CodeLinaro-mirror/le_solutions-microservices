// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// LiteRTEngine — Layer 4 wrapper around the LiteRT C API
//
// Loads a .tflite model and runs inference via the LiteRT CompiledModel API.
// Supports NPU acceleration (Qualcomm QNN dispatch) with CPU fallback.
//
// The LiteRT runtime library (liblitert_runtime_c_api.so) must be present at
// runtime. The dispatch library (libLiteRtDispatch*.so) and compiler plugin
// (libLiteRtQnnPlugin*.so or similar) are located via the directories passed
// to the constructor — these are loaded by LiteRT at runtime via dlopen.
//
// Usage:
//   LiteRTEngine engine("/mnt/models/mobilenet/model.tflite",
//                       "/usr/lib",   // dispatch library directory
//                       "/usr/lib");  // compiler plugin directory
//   auto in_specs  = engine.inputSpecs();
//   auto out_specs = engine.outputSpecs();
//   engine.infer(inputs, input_sizes, outputs, output_sizes);
//
// See docs/unified-inference-service.md for the full design rationale.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Describes a single input or output tensor of the loaded model.
 * Populated from the model's signature 0 at construction time.
 *
 * element_type holds a LiteRtElementType value (int) from litert_model_types.h:
 *   kLiteRtElementTypeFloat32 = 1, kLiteRtElementTypeInt8 = 9, etc.
 * Using int avoids pulling LiteRT headers into this public header.
 */
struct LiteRTTensorSpec {
    std::string           name;         // tensor name from the model signature
    std::vector<int32_t>  shape;        // static dimensions (negative = dynamic)
    int                   element_type; // LiteRtElementType value (int)
};

/**
 * LiteRTEngine — wraps LiteRT CompiledModel inference.
 * Non-copyable, non-movable (owns LiteRT handles).
 */
class LiteRTEngine {
public:
    /**
     * Load a .tflite model and compile it for the target accelerator.
     *
     * @param model_file           Absolute path to the .tflite flatbuffer
     * @param dispatch_lib_dir     Directory containing the LiteRT dispatch
     *                             shared library (e.g. libLiteRtDispatch*.so).
     *                             Passed as kLiteRtEnvOptionTagDispatchLibraryDir.
     * @param compiler_plugin_dir  Directory containing the LiteRT compiler
     *                             plugin shared library (e.g. libLiteRtQnnPlugin*.so).
     *                             Passed as kLiteRtEnvOptionTagCompilerPluginLibraryDir.
     * @param hw_accelerators      Bitmask of LiteRtHwAccelerators values.
     *                             Default: NPU | CPU (NPU with CPU fallback).
     * @throws std::runtime_error on any initialization failure
     */
    LiteRTEngine(const std::string& model_file,
                 const std::string& dispatch_lib_dir,
                 const std::string& compiler_plugin_dir,
                 int hw_accelerators = (1 << 2) | (1 << 0)); // kLiteRtHwAcceleratorNpu | kLiteRtHwAcceleratorCpu

    ~LiteRTEngine();

    // Non-copyable, non-movable (owns LiteRT opaque handles)
    LiteRTEngine(const LiteRTEngine&)            = delete;
    LiteRTEngine& operator=(const LiteRTEngine&) = delete;

    /**
     * Returns the input tensor specifications of the loaded model (signature 0).
     * Call after construction to discover input names, shapes, and element types.
     */
    const std::vector<LiteRTTensorSpec>& inputSpecs() const;

    /**
     * Returns the output tensor specifications of the loaded model (signature 0).
     */
    const std::vector<LiteRTTensorSpec>& outputSpecs() const;

    /**
     * Run inference synchronously.
     *
     * Creates managed LiteRtTensorBuffers from the compiled model's buffer
     * requirements, copies input data in, runs the model, copies output data out,
     * then destroys the per-call buffers.
     *
     * @param input_ptrs   One raw byte buffer per input tensor, in the order
     *                     returned by inputSpecs(). Caller owns the memory.
     * @param input_sizes  Byte sizes of each input buffer.
     * @param output_ptrs  One pre-allocated raw byte buffer per output tensor,
     *                     in the order returned by outputSpecs(). Caller owns.
     * @param output_sizes Byte sizes of each output buffer.
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
