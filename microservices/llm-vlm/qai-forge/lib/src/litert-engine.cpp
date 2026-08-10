// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// LiteRTEngine — Layer 4 wrapper around the LiteRT C API
//
// Inference flow:
//   1. LiteRtCreateEnvironment  — sets dispatch + compiler plugin dirs
//   2. LiteRtCreateModelFromFile — loads .tflite flatbuffer
//   3. LiteRtCreateOptions + LiteRtSetOptionsHardwareAccelerators
//   4. LiteRtCreateCompiledModel — JIT-compiles for NPU (with CPU fallback)
//   5. Enumerate signature 0 inputs/outputs → populate tensor specs
//
// Per-inference:
//   6. LiteRtGetCompiledModelInputBufferRequirements
//   7. LiteRtCreateManagedTensorBufferFromRequirements
//   8. LiteRtLockTensorBuffer / memcpy input / LiteRtUnlockTensorBuffer
//   9. LiteRtRunCompiledModel
//  10. LiteRtLockTensorBuffer / memcpy output / LiteRtUnlockTensorBuffer
//  11. LiteRtDestroyTensorBuffer (per-call buffers)
// ─────────────────────────────────────────────────────────────────────────────

#include "litert-engine.hpp"

// LiteRT C API headers — resolved via LITERT_INCLUDE_PATH at build time.
// The include root is the LiteRT source tree root (contains litert/c/...).
#include "litert/c/litert_any.h"
#include "litert/c/litert_common.h"
#include "litert/c/litert_compiled_model.h"
#include "litert/c/litert_environment.h"
#include "litert/c/litert_environment_options.h"
#include "litert/c/litert_layout.h"
#include "litert/c/litert_model.h"
#include "litert/c/litert_model_types.h"
#include "litert/c/litert_options.h"
#include "litert/c/litert_tensor_buffer.h"
#include "litert/c/litert_tensor_buffer_requirements.h"

#include <cstring>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static void checkStatus(LiteRtStatus status, const char* context) {
    if (status != kLiteRtStatusOk) {
        throw std::runtime_error(std::string("LiteRTEngine: ") + context
                                 + " failed (status=" + std::to_string(status) + ")");
    }
}

// Returns the byte size of one element for a given LiteRtElementType value (as int).
// Values match kLiteRtElementType* from litert/c/litert_model_types.h.
static size_t elementBytes(int et) {
    switch (et) {
        case 1:  // kLiteRtElementTypeFloat32
        case 2:  // kLiteRtElementTypeInt32
        case 16: // kLiteRtElementTypeUInt32
            return 4;
        case 10: // kLiteRtElementTypeFloat16
        case 19: // kLiteRtElementTypeBFloat16
        case 7:  // kLiteRtElementTypeInt16
        case 17: // kLiteRtElementTypeUInt16
            return 2;
        case 9:  // kLiteRtElementTypeInt8
        case 3:  // kLiteRtElementTypeUInt8
        case 6:  // kLiteRtElementTypeBool
            return 1;
        case 11: // kLiteRtElementTypeFloat64
        case 4:  // kLiteRtElementTypeInt64
        case 13: // kLiteRtElementTypeUInt64
            return 8;
        case 18: // kLiteRtElementTypeInt4
        case 20: // kLiteRtElementTypeInt2
            return 1; // packed; treat as 1 byte minimum
        default:
            return 4;
    }
}

// Compute packed byte size from a LiteRtRankedTensorType.
static size_t packedBytes(const LiteRtRankedTensorType& tt) {
    size_t n = elementBytes(static_cast<int>(tt.element_type));
    for (unsigned int i = 0; i < tt.layout.rank; ++i) {
        int32_t dim = tt.layout.dimensions[i];
        if (dim <= 0) dim = 1; // treat dynamic dims as 1 for buffer sizing
        n *= static_cast<size_t>(dim);
    }
    return n;
}

// ─────────────────────────────────────────────────────────────────────────────
// LiteRTEngine::Impl
// ─────────────────────────────────────────────────────────────────────────────

struct LiteRTEngine::Impl {
    LiteRtEnvironment    env             = nullptr;
    LiteRtModel          model           = nullptr;
    LiteRtOptions        options         = nullptr;
    LiteRtCompiledModel  compiled_model  = nullptr;

    std::vector<LiteRTTensorSpec>      input_specs;
    std::vector<LiteRTTensorSpec>      output_specs;

    // Cached ranked tensor types for buffer creation (signature 0)
    std::vector<LiteRtRankedTensorType> input_types;
    std::vector<LiteRtRankedTensorType> output_types;

    ~Impl() { cleanup(); }

    void cleanup() {
        if (compiled_model) { LiteRtDestroyCompiledModel(compiled_model); compiled_model = nullptr; }
        if (options)        { LiteRtDestroyOptions(options);               options        = nullptr; }
        if (model)          { LiteRtDestroyModel(model);                   model          = nullptr; }
        if (env)            { LiteRtDestroyEnvironment(env);               env            = nullptr; }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

LiteRTEngine::LiteRTEngine(const std::string& model_file,
                           const std::string& dispatch_lib_dir,
                           const std::string& compiler_plugin_dir,
                           int hw_accelerators)
    : impl_(std::make_unique<Impl>())
{
    auto& m = *impl_;

    // ── 1. Create environment ─────────────────────────────────────────────────
    // kLiteRtEnvOptionTagCompilerPluginLibraryDir = 0
    // kLiteRtEnvOptionTagDispatchLibraryDir       = 1
    LiteRtEnvOption env_opts[2];

    env_opts[0].tag              = kLiteRtEnvOptionTagCompilerPluginLibraryDir;
    env_opts[0].value.type       = kLiteRtAnyTypeString;
    env_opts[0].value.str_value  = compiler_plugin_dir.c_str();

    env_opts[1].tag              = kLiteRtEnvOptionTagDispatchLibraryDir;
    env_opts[1].value.type       = kLiteRtAnyTypeString;
    env_opts[1].value.str_value  = dispatch_lib_dir.c_str();

    checkStatus(LiteRtCreateEnvironment(2, env_opts, &m.env),
                "LiteRtCreateEnvironment");

    // ── 2. Load model ─────────────────────────────────────────────────────────
    checkStatus(LiteRtCreateModelFromFile(m.env, model_file.c_str(), &m.model),
                "LiteRtCreateModelFromFile");

    // ── 3. Compilation options ────────────────────────────────────────────────
    checkStatus(LiteRtCreateOptions(&m.options),
                "LiteRtCreateOptions");
    checkStatus(LiteRtSetOptionsHardwareAccelerators(
                    m.options,
                    static_cast<LiteRtHwAcceleratorSet>(hw_accelerators)),
                "LiteRtSetOptionsHardwareAccelerators");

    // ── 4. Compile model ──────────────────────────────────────────────────────
    checkStatus(LiteRtCreateCompiledModel(m.env, m.model, m.options, &m.compiled_model),
                "LiteRtCreateCompiledModel");

    // ── 5. Enumerate tensor specs from signature 0 ────────────────────────────
    LiteRtParamIndex num_sigs = 0;
    checkStatus(LiteRtGetNumModelSignatures(m.model, &num_sigs),
                "LiteRtGetNumModelSignatures");
    if (num_sigs == 0)
        throw std::runtime_error("LiteRTEngine: model has no signatures");

    LiteRtSignature sig = nullptr;
    checkStatus(LiteRtGetModelSignature(m.model, 0, &sig),
                "LiteRtGetModelSignature");

    // Inputs
    LiteRtParamIndex num_inputs = 0;
    checkStatus(LiteRtGetNumSignatureInputs(sig, &num_inputs),
                "LiteRtGetNumSignatureInputs");

    for (LiteRtParamIndex i = 0; i < num_inputs; ++i) {
        const char* name = nullptr;
        checkStatus(LiteRtGetSignatureInputName(sig, i, &name),
                    "LiteRtGetSignatureInputName");

        LiteRtTensor tensor = nullptr;
        checkStatus(LiteRtGetSignatureInputTensorByIndex(sig, i, &tensor),
                    "LiteRtGetSignatureInputTensorByIndex");

        LiteRtRankedTensorType tt{};
        checkStatus(LiteRtGetRankedTensorType(tensor, &tt),
                    "LiteRtGetRankedTensorType (input)");

        LiteRTTensorSpec spec;
        spec.name         = name ? name : ("input_" + std::to_string(i));
        spec.element_type = static_cast<int>(tt.element_type);
        for (unsigned int d = 0; d < tt.layout.rank; ++d)
            spec.shape.push_back(tt.layout.dimensions[d]);

        m.input_specs.push_back(std::move(spec));
        m.input_types.push_back(tt);
    }

    // Outputs
    LiteRtParamIndex num_outputs = 0;
    checkStatus(LiteRtGetNumSignatureOutputs(sig, &num_outputs),
                "LiteRtGetNumSignatureOutputs");

    for (LiteRtParamIndex i = 0; i < num_outputs; ++i) {
        const char* name = nullptr;
        checkStatus(LiteRtGetSignatureOutputName(sig, i, &name),
                    "LiteRtGetSignatureOutputName");

        LiteRtTensor tensor = nullptr;
        checkStatus(LiteRtGetSignatureOutputTensorByIndex(sig, i, &tensor),
                    "LiteRtGetSignatureOutputTensorByIndex");

        LiteRtRankedTensorType tt{};
        checkStatus(LiteRtGetRankedTensorType(tensor, &tt),
                    "LiteRtGetRankedTensorType (output)");

        LiteRTTensorSpec spec;
        spec.name         = name ? name : ("output_" + std::to_string(i));
        spec.element_type = static_cast<int>(tt.element_type);
        for (unsigned int d = 0; d < tt.layout.rank; ++d)
            spec.shape.push_back(tt.layout.dimensions[d]);

        m.output_specs.push_back(std::move(spec));
        m.output_types.push_back(tt);
    }
}

LiteRTEngine::~LiteRTEngine() = default;

// ─────────────────────────────────────────────────────────────────────────────
// Accessors
// ─────────────────────────────────────────────────────────────────────────────

const std::vector<LiteRTTensorSpec>& LiteRTEngine::inputSpecs() const {
    return impl_->input_specs;
}

const std::vector<LiteRTTensorSpec>& LiteRTEngine::outputSpecs() const {
    return impl_->output_specs;
}

// ─────────────────────────────────────────────────────────────────────────────
// infer()
// ─────────────────────────────────────────────────────────────────────────────

void LiteRTEngine::infer(const std::vector<const uint8_t*>& input_ptrs,
                         const std::vector<size_t>&          input_sizes,
                         const std::vector<uint8_t*>&         output_ptrs,
                         const std::vector<size_t>&           output_sizes)
{
    auto& m = *impl_;

    const size_t n_in  = m.input_specs.size();
    const size_t n_out = m.output_specs.size();

    if (input_ptrs.size() != n_in)
        throw std::runtime_error("LiteRTEngine::infer: wrong number of inputs");
    if (output_ptrs.size() != n_out)
        throw std::runtime_error("LiteRTEngine::infer: wrong number of outputs");

    // ── Allocate input tensor buffers ─────────────────────────────────────────
    std::vector<LiteRtTensorBuffer> in_bufs(n_in, nullptr);
    for (size_t i = 0; i < n_in; ++i) {
        LiteRtTensorBufferRequirements req = nullptr;
        checkStatus(LiteRtGetCompiledModelInputBufferRequirements(
                        m.compiled_model, 0,
                        static_cast<LiteRtParamIndex>(i), &req),
                    "LiteRtGetCompiledModelInputBufferRequirements");

        checkStatus(LiteRtCreateManagedTensorBufferFromRequirements(
                        m.env, &m.input_types[i], req, &in_bufs[i]),
                    "LiteRtCreateManagedTensorBufferFromRequirements (input)");

        // Copy caller data into the buffer
        void* host_ptr = nullptr;
        checkStatus(LiteRtLockTensorBuffer(in_bufs[i], &host_ptr,
                                           kLiteRtTensorBufferLockModeWrite),
                    "LiteRtLockTensorBuffer (input write)");

        size_t copy_bytes = input_sizes[i];
        size_t buf_bytes  = packedBytes(m.input_types[i]);
        if (copy_bytes > buf_bytes) copy_bytes = buf_bytes;
        std::memcpy(host_ptr, input_ptrs[i], copy_bytes);

        checkStatus(LiteRtUnlockTensorBuffer(in_bufs[i]),
                    "LiteRtUnlockTensorBuffer (input)");
    }

    // ── Allocate output tensor buffers ────────────────────────────────────────
    std::vector<LiteRtTensorBuffer> out_bufs(n_out, nullptr);
    for (size_t i = 0; i < n_out; ++i) {
        LiteRtTensorBufferRequirements req = nullptr;
        checkStatus(LiteRtGetCompiledModelOutputBufferRequirements(
                        m.compiled_model, 0,
                        static_cast<LiteRtParamIndex>(i), &req),
                    "LiteRtGetCompiledModelOutputBufferRequirements");

        checkStatus(LiteRtCreateManagedTensorBufferFromRequirements(
                        m.env, &m.output_types[i], req, &out_bufs[i]),
                    "LiteRtCreateManagedTensorBufferFromRequirements (output)");
    }

    // ── Run inference ─────────────────────────────────────────────────────────
    LiteRtStatus run_status = LiteRtRunCompiledModel(
        m.compiled_model, 0,
        static_cast<size_t>(n_in),  in_bufs.data(),
        static_cast<size_t>(n_out), out_bufs.data());

    // ── Copy output data out before destroying buffers ────────────────────────
    for (size_t i = 0; i < n_out; ++i) {
        void* host_ptr = nullptr;
        LiteRtStatus lock_st = LiteRtLockTensorBuffer(out_bufs[i], &host_ptr,
                                                       kLiteRtTensorBufferLockModeRead);
        if (lock_st == kLiteRtStatusOk && host_ptr) {
            size_t copy_bytes = output_sizes[i];
            size_t buf_bytes  = packedBytes(m.output_types[i]);
            if (copy_bytes > buf_bytes) copy_bytes = buf_bytes;
            std::memcpy(output_ptrs[i], host_ptr, copy_bytes);
            LiteRtUnlockTensorBuffer(out_bufs[i]);
        }
        LiteRtDestroyTensorBuffer(out_bufs[i]);
    }
    for (size_t i = 0; i < n_in; ++i)
        LiteRtDestroyTensorBuffer(in_bufs[i]);

    // Check run status after cleanup so buffers are always freed
    checkStatus(run_status, "LiteRtRunCompiledModel");
}
