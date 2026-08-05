// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// SNPEEngine — Layer 4 wrapper around the SNPE C API
//
// Translated from the GStreamer ml-snpe-engine.c reference implementation.
// Uses the SNPE C API (not the C++ API) loaded via dlopen at runtime.
//
// SNPE API access pattern:
//   1. dlopen("libSNPE.so") → load all function pointers
//   2. DlContainerOpen(model_file) → container
//   3. SNPEBuilderCreate(container) → builder
//   4. Configure: runtime order, performance profile, user buffers
//   5. SNPEBuilderBuild() → interpreter
//   6. Setup UserBufferMap with placeholder buffers (empty, address set later)
//   7. At inference: IUserBuffer_SetBufferAddress(buf, data_ptr)
//   8. SNPE_ExecuteUserBuffers(interpreter, inputs, outputs)
// ─────────────────────────────────────────────────────────────────────────────

#include "snpe-engine.hpp"

#include <dlfcn.h>
#include <stdexcept>
#include <cstring>
#include <iostream>
#include <numeric>
#include <map>

// SNPE C API headers — resolved via SNPE_INCLUDE_PATH at build time
#include <DlContainer/DlContainer.h>
#include <DlSystem/IUserBuffer.h>
#include <SNPE/SNPE.h>
#include <SNPE/SNPEUtil.h>
#include <SNPE/SNPEBuilder.h>

// ─────────────────────────────────────────────────────────────────────────────
// SNPE function pointer typedefs (C API)
// ─────────────────────────────────────────────────────────────────────────────

// All SNPE C API functions are loaded via dlopen to avoid hard linking.
// This mirrors the GStreamer ml-snpe-engine.c approach.

struct SNPEFunctions {
    // DlContainer
    Snpe_DlContainer_Handle_t (*DlContainerOpen)(const char*) = nullptr;
    Snpe_ErrorCode_t (*DlContainerDelete)(Snpe_DlContainer_Handle_t) = nullptr;

    // SNPEBuilder
    Snpe_SNPEBuilder_Handle_t (*SNPEBuilderCreate)(Snpe_DlContainer_Handle_t) = nullptr;
    Snpe_ErrorCode_t (*SNPEBuilderDelete)(Snpe_SNPEBuilder_Handle_t) = nullptr;
    Snpe_ErrorCode_t (*SNPEBuilderSetOutputTensors)(Snpe_SNPEBuilder_Handle_t, Snpe_StringList_Handle_t) = nullptr;
    Snpe_ErrorCode_t (*SNPEBuilderSetRuntimeProcessorOrder)(Snpe_SNPEBuilder_Handle_t, Snpe_RuntimeList_Handle_t) = nullptr;
    Snpe_ErrorCode_t (*SNPEBuilderSetUseUserSuppliedBuffers)(Snpe_SNPEBuilder_Handle_t, int) = nullptr;
    Snpe_ErrorCode_t (*SNPEBuilderSetPerformanceProfile)(Snpe_SNPEBuilder_Handle_t, Snpe_PerformanceProfile_t) = nullptr;
    Snpe_SNPE_Handle_t (*SNPEBuilderBuild)(Snpe_SNPEBuilder_Handle_t) = nullptr;

    // SNPE interpreter
    Snpe_ErrorCode_t (*SNPE_Delete)(Snpe_SNPE_Handle_t) = nullptr;
    Snpe_StringList_Handle_t (*SNPE_GetInputTensorNames)(Snpe_SNPE_Handle_t) = nullptr;
    Snpe_StringList_Handle_t (*SNPE_GetOutputTensorNames)(Snpe_SNPE_Handle_t) = nullptr;
    Snpe_IBufferAttributes_Handle_t (*SNPE_GetInputOutputBufferAttributes)(Snpe_SNPE_Handle_t, const char*) = nullptr;
    Snpe_ErrorCode_t (*SNPE_ExecuteUserBuffers)(Snpe_SNPE_Handle_t, Snpe_UserBufferMap_Handle_t, Snpe_UserBufferMap_Handle_t) = nullptr;

    // RuntimeList
    Snpe_RuntimeList_Handle_t (*RuntimeListCreate)() = nullptr;
    Snpe_ErrorCode_t (*RuntimeListDelete)(Snpe_RuntimeList_Handle_t) = nullptr;
    Snpe_ErrorCode_t (*RuntimeListAdd)(Snpe_RuntimeList_Handle_t, Snpe_Runtime_t) = nullptr;

    // StringList
    Snpe_StringList_Handle_t (*StringListCreate)() = nullptr;
    Snpe_StringList_Handle_t (*StringListCreateCopy)(Snpe_StringList_Handle_t) = nullptr;
    Snpe_ErrorCode_t (*StringListDelete)(Snpe_StringList_Handle_t) = nullptr;
    Snpe_ErrorCode_t (*StringListAppend)(Snpe_StringList_Handle_t, const char*) = nullptr;
    size_t (*StringListSize)(Snpe_StringList_Handle_t) = nullptr;
    const char* (*StringListAt)(Snpe_StringList_Handle_t, size_t) = nullptr;

    // IBufferAttributes
    Snpe_ErrorCode_t (*IBufferAttributesDelete)(Snpe_IBufferAttributes_Handle_t) = nullptr;
    Snpe_UserBufferEncoding_ElementType_t (*IBufferAttributesGetEncodingType)(Snpe_IBufferAttributes_Handle_t) = nullptr;
    Snpe_TensorShape_Handle_t (*IBufferAttributesGetDims)(Snpe_IBufferAttributes_Handle_t) = nullptr;
    Snpe_UserBufferEncoding_Handle_t (*IBufferAttributesGetEncoding)(Snpe_IBufferAttributes_Handle_t) = nullptr;

    // TensorShape
    Snpe_ErrorCode_t (*TensorShapeDelete)(Snpe_TensorShape_Handle_t) = nullptr;
    size_t (*TensorShapeRank)(Snpe_TensorShape_Handle_t) = nullptr;
    Snpe_TensorShape_Handle_t (*TensorShapeCreateDimsSize)(const size_t*, size_t) = nullptr;
    const size_t* (*TensorShapeGetDimensions)(Snpe_TensorShape_Handle_t) = nullptr;

    // UserBufferMap
    Snpe_UserBufferMap_Handle_t (*UserBufferMapCreate)() = nullptr;
    Snpe_ErrorCode_t (*UserBufferMapDelete)(Snpe_UserBufferMap_Handle_t) = nullptr;
    Snpe_ErrorCode_t (*UserBufferMapAdd)(Snpe_UserBufferMap_Handle_t, const char*, Snpe_IUserBuffer_Handle_t) = nullptr;
    Snpe_ErrorCode_t (*UserBufferMapRemove)(Snpe_UserBufferMap_Handle_t, const char*) = nullptr;
    Snpe_IUserBuffer_Handle_t (*UserBufferMapGet)(Snpe_UserBufferMap_Handle_t, const char*) = nullptr;

    // IUserBuffer
    Snpe_ErrorCode_t (*IUserBufferDelete)(Snpe_IUserBuffer_Handle_t) = nullptr;
    int (*IUserBufferSetBufferAddress)(Snpe_IUserBuffer_Handle_t, void*) = nullptr;

    // Util
    Snpe_IUserBuffer_Handle_t (*UtilCreateUserBuffer)(void*, size_t, Snpe_TensorShape_Handle_t, Snpe_IUserBuffer_Handle_t) = nullptr;

    // Encoding
    Snpe_UserBufferEncoding_Handle_t (*UserBufferEncodingFloatCreate)() = nullptr;
    Snpe_ErrorCode_t (*UserBufferEncodingFloatDelete)(Snpe_UserBufferEncoding_Handle_t) = nullptr;
};

// ─────────────────────────────────────────────────────────────────────────────
// SNPEEngine::Impl
// ─────────────────────────────────────────────────────────────────────────────

struct SNPEEngine::Impl {
    void*                          libhandle    = nullptr;
    SNPEFunctions                  fn;

    Snpe_DlContainer_Handle_t      container    = nullptr;
    Snpe_SNPEBuilder_Handle_t      builder      = nullptr;
    Snpe_SNPE_Handle_t             interpreter  = nullptr;

    Snpe_StringList_Handle_t       outnames     = nullptr;
    Snpe_UserBufferMap_Handle_t    inputs       = nullptr;
    Snpe_UserBufferMap_Handle_t    outputs      = nullptr;

    // Per-tensor user buffer handles (for address update at inference time)
    std::map<std::string, Snpe_IUserBuffer_Handle_t> usrbuffers;

    std::vector<SNPETensorSpec>    input_specs;
    std::vector<SNPETensorSpec>    output_specs;

    ~Impl() { cleanup(); }

    void cleanup() {
        // Free user buffers
        for (auto& [name, buf] : usrbuffers) {
            if (buf && fn.IUserBufferDelete)
                fn.IUserBufferDelete(buf);
        }
        usrbuffers.clear();

        if (outputs && fn.UserBufferMapDelete) fn.UserBufferMapDelete(outputs);
        if (inputs  && fn.UserBufferMapDelete) fn.UserBufferMapDelete(inputs);
        if (outnames && fn.StringListDelete)   fn.StringListDelete(outnames);
        if (interpreter && fn.SNPE_Delete)     fn.SNPE_Delete(interpreter);
        if (builder && fn.SNPEBuilderDelete)   fn.SNPEBuilderDelete(builder);
        if (container && fn.DlContainerDelete) fn.DlContainerDelete(container);
        if (libhandle) { dlclose(libhandle); libhandle = nullptr; }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static void* loadSym(void* handle, const char* name) {
    void* sym = dlsym(handle, name);
    if (!sym)
        throw std::runtime_error(std::string("SNPEEngine: symbol not found: ") + name
                                 + " (" + dlerror() + ")");
    return sym;
}

static std::string snpeEncodingToString(Snpe_UserBufferEncoding_ElementType_t t) {
    switch (t) {
        case SNPE_USERBUFFERENCODING_ELEMENTTYPE_FLOAT:   return "float32";
        case SNPE_USERBUFFERENCODING_ELEMENTTYPE_FLOAT16: return "float16";
        case SNPE_USERBUFFERENCODING_ELEMENTTYPE_INT8:    return "int8";
        case SNPE_USERBUFFERENCODING_ELEMENTTYPE_UINT8:
        case SNPE_USERBUFFERENCODING_ELEMENTTYPE_UNSIGNED8BIT:
        case SNPE_USERBUFFERENCODING_ELEMENTTYPE_TF8:     return "uint8";
        case SNPE_USERBUFFERENCODING_ELEMENTTYPE_INT16:   return "int16";
        case SNPE_USERBUFFERENCODING_ELEMENTTYPE_UINT16:
        case SNPE_USERBUFFERENCODING_ELEMENTTYPE_TF16:    return "uint16";
        case SNPE_USERBUFFERENCODING_ELEMENTTYPE_INT32:   return "int32";
        case SNPE_USERBUFFERENCODING_ELEMENTTYPE_UINT32:  return "uint32";
        case SNPE_USERBUFFERENCODING_ELEMENTTYPE_INT64:   return "int64";
        case SNPE_USERBUFFERENCODING_ELEMENTTYPE_UINT64:  return "uint64";
        default:                                          return "unknown";
    }
}

static size_t snpeEncodingBytes(Snpe_UserBufferEncoding_ElementType_t t) {
    switch (t) {
        case SNPE_USERBUFFERENCODING_ELEMENTTYPE_FLOAT:   return 4;
        case SNPE_USERBUFFERENCODING_ELEMENTTYPE_FLOAT16:
        case SNPE_USERBUFFERENCODING_ELEMENTTYPE_INT16:
        case SNPE_USERBUFFERENCODING_ELEMENTTYPE_UINT16:
        case SNPE_USERBUFFERENCODING_ELEMENTTYPE_TF16:    return 2;
        case SNPE_USERBUFFERENCODING_ELEMENTTYPE_INT8:
        case SNPE_USERBUFFERENCODING_ELEMENTTYPE_UINT8:
        case SNPE_USERBUFFERENCODING_ELEMENTTYPE_UNSIGNED8BIT:
        case SNPE_USERBUFFERENCODING_ELEMENTTYPE_TF8:     return 1;
        case SNPE_USERBUFFERENCODING_ELEMENTTYPE_INT32:
        case SNPE_USERBUFFERENCODING_ELEMENTTYPE_UINT32:  return 4;
        case SNPE_USERBUFFERENCODING_ELEMENTTYPE_INT64:
        case SNPE_USERBUFFERENCODING_ELEMENTTYPE_UINT64:  return 8;
        default:                                          return 4;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// SNPEEngine constructor
// ─────────────────────────────────────────────────────────────────────────────

SNPEEngine::SNPEEngine(const std::string&              model_file,
                       const std::string&              delegate,
                       const std::vector<std::string>& output_tensors)
    : impl_(std::make_unique<Impl>())
{
    auto& m = *impl_;
    auto& fn = m.fn;

    // ── 1. Load libSNPE.so and all function pointers ──────────────────────────
    m.libhandle = dlopen("libSNPE.so", RTLD_NOW | RTLD_LOCAL);
    if (!m.libhandle)
        throw std::runtime_error("SNPEEngine: failed to open libSNPE.so: "
                                 + std::string(dlerror()));

    fn.DlContainerOpen   = (decltype(fn.DlContainerOpen))  loadSym(m.libhandle, "Snpe_DlContainer_Open");
    fn.DlContainerDelete = (decltype(fn.DlContainerDelete)) loadSym(m.libhandle, "Snpe_DlContainer_Delete");

    fn.SNPEBuilderCreate                  = (decltype(fn.SNPEBuilderCreate))                  loadSym(m.libhandle, "Snpe_SNPEBuilder_Create");
    fn.SNPEBuilderDelete                  = (decltype(fn.SNPEBuilderDelete))                  loadSym(m.libhandle, "Snpe_SNPEBuilder_Delete");
    fn.SNPEBuilderSetOutputTensors        = (decltype(fn.SNPEBuilderSetOutputTensors))        loadSym(m.libhandle, "Snpe_SNPEBuilder_SetOutputTensors");
    fn.SNPEBuilderSetRuntimeProcessorOrder= (decltype(fn.SNPEBuilderSetRuntimeProcessorOrder))loadSym(m.libhandle, "Snpe_SNPEBuilder_SetRuntimeProcessorOrder");
    fn.SNPEBuilderSetUseUserSuppliedBuffers=(decltype(fn.SNPEBuilderSetUseUserSuppliedBuffers))loadSym(m.libhandle, "Snpe_SNPEBuilder_SetUseUserSuppliedBuffers");
    fn.SNPEBuilderSetPerformanceProfile   = (decltype(fn.SNPEBuilderSetPerformanceProfile))   loadSym(m.libhandle, "Snpe_SNPEBuilder_SetPerformanceProfile");
    fn.SNPEBuilderBuild                   = (decltype(fn.SNPEBuilderBuild))                   loadSym(m.libhandle, "Snpe_SNPEBuilder_Build");

    fn.SNPE_Delete                        = (decltype(fn.SNPE_Delete))                        loadSym(m.libhandle, "Snpe_SNPE_Delete");
    fn.SNPE_GetInputTensorNames           = (decltype(fn.SNPE_GetInputTensorNames))           loadSym(m.libhandle, "Snpe_SNPE_GetInputTensorNames");
    fn.SNPE_GetOutputTensorNames          = (decltype(fn.SNPE_GetOutputTensorNames))          loadSym(m.libhandle, "Snpe_SNPE_GetOutputTensorNames");
    fn.SNPE_GetInputOutputBufferAttributes= (decltype(fn.SNPE_GetInputOutputBufferAttributes))loadSym(m.libhandle, "Snpe_SNPE_GetInputOutputBufferAttributes");
    fn.SNPE_ExecuteUserBuffers            = (decltype(fn.SNPE_ExecuteUserBuffers))            loadSym(m.libhandle, "Snpe_SNPE_ExecuteUserBuffers");

    fn.RuntimeListCreate  = (decltype(fn.RuntimeListCreate))  loadSym(m.libhandle, "Snpe_RuntimeList_Create");
    fn.RuntimeListDelete  = (decltype(fn.RuntimeListDelete))  loadSym(m.libhandle, "Snpe_RuntimeList_Delete");
    fn.RuntimeListAdd     = (decltype(fn.RuntimeListAdd))     loadSym(m.libhandle, "Snpe_RuntimeList_Add");

    fn.StringListCreate     = (decltype(fn.StringListCreate))     loadSym(m.libhandle, "Snpe_StringList_Create");
    fn.StringListCreateCopy = (decltype(fn.StringListCreateCopy)) loadSym(m.libhandle, "Snpe_StringList_CreateCopy");
    fn.StringListDelete     = (decltype(fn.StringListDelete))     loadSym(m.libhandle, "Snpe_StringList_Delete");
    fn.StringListAppend     = (decltype(fn.StringListAppend))     loadSym(m.libhandle, "Snpe_StringList_Append");
    fn.StringListSize       = (decltype(fn.StringListSize))       loadSym(m.libhandle, "Snpe_StringList_Size");
    fn.StringListAt         = (decltype(fn.StringListAt))         loadSym(m.libhandle, "Snpe_StringList_At");

    fn.IBufferAttributesDelete          = (decltype(fn.IBufferAttributesDelete))          loadSym(m.libhandle, "Snpe_IBufferAttributes_Delete");
    fn.IBufferAttributesGetEncodingType = (decltype(fn.IBufferAttributesGetEncodingType)) loadSym(m.libhandle, "Snpe_IBufferAttributes_GetEncodingType");
    fn.IBufferAttributesGetDims         = (decltype(fn.IBufferAttributesGetDims))         loadSym(m.libhandle, "Snpe_IBufferAttributes_GetDims");
    fn.IBufferAttributesGetEncoding     = (decltype(fn.IBufferAttributesGetEncoding))     loadSym(m.libhandle, "Snpe_IBufferAttributes_GetEncoding_Ref");

    fn.TensorShapeDelete        = (decltype(fn.TensorShapeDelete))        loadSym(m.libhandle, "Snpe_TensorShape_Delete");
    fn.TensorShapeRank          = (decltype(fn.TensorShapeRank))          loadSym(m.libhandle, "Snpe_TensorShape_Rank");
    fn.TensorShapeCreateDimsSize= (decltype(fn.TensorShapeCreateDimsSize))loadSym(m.libhandle, "Snpe_TensorShape_CreateDimsSize");
    fn.TensorShapeGetDimensions = (decltype(fn.TensorShapeGetDimensions)) loadSym(m.libhandle, "Snpe_TensorShape_GetDimensions");

    fn.UserBufferMapCreate  = (decltype(fn.UserBufferMapCreate))  loadSym(m.libhandle, "Snpe_UserBufferMap_Create");
    fn.UserBufferMapDelete  = (decltype(fn.UserBufferMapDelete))  loadSym(m.libhandle, "Snpe_UserBufferMap_Delete");
    fn.UserBufferMapAdd     = (decltype(fn.UserBufferMapAdd))     loadSym(m.libhandle, "Snpe_UserBufferMap_Add");
    fn.UserBufferMapRemove  = (decltype(fn.UserBufferMapRemove))  loadSym(m.libhandle, "Snpe_UserBufferMap_Remove");
    fn.UserBufferMapGet     = (decltype(fn.UserBufferMapGet))     loadSym(m.libhandle, "Snpe_UserBufferMap_GetUserBuffer_Ref");

    fn.IUserBufferDelete           = (decltype(fn.IUserBufferDelete))           loadSym(m.libhandle, "Snpe_IUserBuffer_Delete");
    fn.IUserBufferSetBufferAddress = (decltype(fn.IUserBufferSetBufferAddress)) loadSym(m.libhandle, "Snpe_IUserBuffer_SetBufferAddress");

    fn.UtilCreateUserBuffer = (decltype(fn.UtilCreateUserBuffer)) loadSym(m.libhandle, "Snpe_Util_CreateUserBuffer");

    fn.UserBufferEncodingFloatCreate = (decltype(fn.UserBufferEncodingFloatCreate)) loadSym(m.libhandle, "Snpe_UserBufferEncodingFloat_Create");
    fn.UserBufferEncodingFloatDelete = (decltype(fn.UserBufferEncodingFloatDelete)) loadSym(m.libhandle, "Snpe_UserBufferEncodingFloat_Delete");

    // ── 2. Open DLC container ─────────────────────────────────────────────────
    m.container = fn.DlContainerOpen(model_file.c_str());
    if (!m.container)
        throw std::runtime_error("SNPEEngine: failed to open model: " + model_file);

    // ── 3. Create builder ─────────────────────────────────────────────────────
    m.builder = fn.SNPEBuilderCreate(m.container);
    if (!m.builder)
        throw std::runtime_error("SNPEEngine: SNPEBuilderCreate failed");

    // ── 4. Configure runtime order ────────────────────────────────────────────
    Snpe_RuntimeList_Handle_t rtlist = fn.RuntimeListCreate();
    if (!rtlist)
        throw std::runtime_error("SNPEEngine: RuntimeListCreate failed");

    if (delegate == "dsp")
        fn.RuntimeListAdd(rtlist, SNPE_RUNTIME_DSP);
    else if (delegate == "gpu")
        fn.RuntimeListAdd(rtlist, SNPE_RUNTIME_GPU);
    else if (delegate == "aip")
        fn.RuntimeListAdd(rtlist, SNPE_RUNTIME_AIP_FIXED8_TF);
    // Always add CPU as fallback
    fn.RuntimeListAdd(rtlist, SNPE_RUNTIME_CPU);

    fn.SNPEBuilderSetRuntimeProcessorOrder(m.builder, rtlist);
    fn.RuntimeListDelete(rtlist);

    // ── 5. Configure output tensors ───────────────────────────────────────────
    Snpe_StringList_Handle_t out_strlist = fn.StringListCreate();
    for (const auto& name : output_tensors)
        fn.StringListAppend(out_strlist, name.c_str());

    if (!output_tensors.empty())
        fn.SNPEBuilderSetOutputTensors(m.builder, out_strlist);

    // ── 6. Configure performance + user buffers ───────────────────────────────
    fn.SNPEBuilderSetPerformanceProfile(m.builder, SNPE_PERFORMANCE_PROFILE_HIGH_PERFORMANCE);
    fn.SNPEBuilderSetUseUserSuppliedBuffers(m.builder, 1);

    // ── 7. Build interpreter ──────────────────────────────────────────────────
    m.interpreter = fn.SNPEBuilderBuild(m.builder);
    if (!m.interpreter) {
        fn.StringListDelete(out_strlist);
        throw std::runtime_error("SNPEEngine: SNPEBuilderBuild failed");
    }

    // ── 8. Get output tensor names ────────────────────────────────────────────
    if (!output_tensors.empty()) {
        m.outnames = fn.StringListCreateCopy(out_strlist);
    } else {
        m.outnames = fn.SNPE_GetOutputTensorNames(m.interpreter);
    }
    fn.StringListDelete(out_strlist);

    if (!m.outnames)
        throw std::runtime_error("SNPEEngine: failed to get output tensor names");

    // ── 9. Create input UserBufferMap ─────────────────────────────────────────
    m.inputs = fn.UserBufferMapCreate();
    if (!m.inputs)
        throw std::runtime_error("SNPEEngine: UserBufferMapCreate (inputs) failed");

    Snpe_StringList_Handle_t in_names = fn.SNPE_GetInputTensorNames(m.interpreter);
    if (!in_names)
        throw std::runtime_error("SNPEEngine: SNPE_GetInputTensorNames failed");

    size_t n_inputs = fn.StringListSize(in_names);
    for (size_t i = 0; i < n_inputs; ++i) {
        const char* name = fn.StringListAt(in_names, i);

        Snpe_IBufferAttributes_Handle_t attribs =
            fn.SNPE_GetInputOutputBufferAttributes(m.interpreter, name);
        if (!attribs) {
            fn.StringListDelete(in_names);
            throw std::runtime_error(std::string("SNPEEngine: no attribs for input: ") + name);
        }

        auto enc_type = fn.IBufferAttributesGetEncodingType(attribs);
        Snpe_TensorShape_Handle_t shape = fn.IBufferAttributesGetDims(attribs);
        const size_t* dims = fn.TensorShapeGetDimensions(shape);
        size_t rank = fn.TensorShapeRank(shape);

        // Build tensor spec
        SNPETensorSpec spec;
        spec.name  = name;
        spec.dtype = snpeEncodingToString(enc_type);
        spec.bytes = snpeEncodingBytes(enc_type);
        for (size_t d = 0; d < rank; ++d) {
            spec.shape.push_back(dims[d]);
            spec.bytes *= dims[d];
        }
        m.input_specs.push_back(spec);

        // Build strides
        std::vector<size_t> strides(rank);
        strides[rank - 1] = snpeEncodingBytes(enc_type);
        for (int d = (int)rank - 2; d >= 0; --d)
            strides[d] = dims[d + 1] * strides[d + 1];

        Snpe_TensorShape_Handle_t stride_shape =
            fn.TensorShapeCreateDimsSize(strides.data(), rank);
        Snpe_UserBufferEncoding_Handle_t encoding =
            fn.IBufferAttributesGetEncoding(attribs);

        // Create placeholder buffer (address set at inference time)
        Snpe_IUserBuffer_Handle_t usrbuf =
            fn.UtilCreateUserBuffer(nullptr, spec.bytes, stride_shape, encoding);

        fn.TensorShapeDelete(stride_shape);
        fn.TensorShapeDelete(shape);
        fn.IBufferAttributesDelete(attribs);

        if (!usrbuf) {
            fn.StringListDelete(in_names);
            throw std::runtime_error(std::string("SNPEEngine: UtilCreateUserBuffer failed for: ") + name);
        }

        m.usrbuffers[name] = usrbuf;
        fn.UserBufferMapAdd(m.inputs, name, usrbuf);
    }
    fn.StringListDelete(in_names);

    // ── 10. Create output UserBufferMap ───────────────────────────────────────
    m.outputs = fn.UserBufferMapCreate();
    if (!m.outputs)
        throw std::runtime_error("SNPEEngine: UserBufferMapCreate (outputs) failed");

    size_t n_outputs = fn.StringListSize(m.outnames);
    for (size_t i = 0; i < n_outputs; ++i) {
        const char* name = fn.StringListAt(m.outnames, i);

        Snpe_IBufferAttributes_Handle_t attribs =
            fn.SNPE_GetInputOutputBufferAttributes(m.interpreter, name);
        if (!attribs)
            throw std::runtime_error(std::string("SNPEEngine: no attribs for output: ") + name);

        auto enc_type = fn.IBufferAttributesGetEncodingType(attribs);
        Snpe_TensorShape_Handle_t shape = fn.IBufferAttributesGetDims(attribs);
        const size_t* dims = fn.TensorShapeGetDimensions(shape);
        size_t rank = fn.TensorShapeRank(shape);

        SNPETensorSpec spec;
        spec.name  = name;
        spec.dtype = snpeEncodingToString(enc_type);
        spec.bytes = snpeEncodingBytes(enc_type);
        for (size_t d = 0; d < rank; ++d) {
            spec.shape.push_back(dims[d]);
            spec.bytes *= dims[d];
        }
        m.output_specs.push_back(spec);

        std::vector<size_t> strides(rank);
        strides[rank - 1] = snpeEncodingBytes(enc_type);
        for (int d = (int)rank - 2; d >= 0; --d)
            strides[d] = dims[d + 1] * strides[d + 1];

        Snpe_TensorShape_Handle_t stride_shape =
            fn.TensorShapeCreateDimsSize(strides.data(), rank);

        // Use float encoding for output (SNPE outputs are typically float)
        Snpe_UserBufferEncoding_Handle_t float_enc = fn.UserBufferEncodingFloatCreate();
        Snpe_IUserBuffer_Handle_t usrbuf =
            fn.UtilCreateUserBuffer(nullptr, spec.bytes, stride_shape, float_enc);
        fn.UserBufferEncodingFloatDelete(float_enc);

        fn.TensorShapeDelete(stride_shape);
        fn.TensorShapeDelete(shape);
        fn.IBufferAttributesDelete(attribs);

        if (!usrbuf)
            throw std::runtime_error(std::string("SNPEEngine: UtilCreateUserBuffer failed for output: ") + name);

        m.usrbuffers[name] = usrbuf;
        fn.UserBufferMapAdd(m.outputs, name, usrbuf);
    }
}

SNPEEngine::~SNPEEngine() = default;

// ─────────────────────────────────────────────────────────────────────────────
// Accessors
// ─────────────────────────────────────────────────────────────────────────────

const std::vector<SNPETensorSpec>& SNPEEngine::inputSpecs() const {
    return impl_->input_specs;
}

const std::vector<SNPETensorSpec>& SNPEEngine::outputSpecs() const {
    return impl_->output_specs;
}

// ─────────────────────────────────────────────────────────────────────────────
// infer()
// ─────────────────────────────────────────────────────────────────────────────

void SNPEEngine::infer(const std::vector<const uint8_t*>& input_ptrs,
                       const std::vector<size_t>&          /*input_sizes*/,
                       const std::vector<uint8_t*>&         output_ptrs,
                       const std::vector<size_t>&           /*output_sizes*/)
{
    auto& m = *impl_;
    auto& fn = m.fn;

    if (input_ptrs.size() != m.input_specs.size())
        throw std::runtime_error("SNPEEngine::infer: wrong number of inputs");
    if (output_ptrs.size() != m.output_specs.size())
        throw std::runtime_error("SNPEEngine::infer: wrong number of outputs");

    // Update input buffer addresses
    for (size_t i = 0; i < m.input_specs.size(); ++i) {
        const std::string& name = m.input_specs[i].name;
        Snpe_IUserBuffer_Handle_t buf = fn.UserBufferMapGet(m.inputs, name.c_str());
        fn.IUserBufferSetBufferAddress(buf, const_cast<uint8_t*>(input_ptrs[i]));
    }

    // Update output buffer addresses
    for (size_t i = 0; i < m.output_specs.size(); ++i) {
        const std::string& name = m.output_specs[i].name;
        Snpe_IUserBuffer_Handle_t buf = fn.UserBufferMapGet(m.outputs, name.c_str());
        fn.IUserBufferSetBufferAddress(buf, output_ptrs[i]);
    }

    // Execute
    Snpe_ErrorCode_t err = fn.SNPE_ExecuteUserBuffers(
        m.interpreter, m.inputs, m.outputs);

    if (err != SNPE_SUCCESS)
        throw std::runtime_error("SNPEEngine::infer: SNPE_ExecuteUserBuffers failed, error="
                                 + std::to_string(static_cast<int>(err)));
}
