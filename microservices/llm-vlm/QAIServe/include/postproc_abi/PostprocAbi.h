// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// PostprocAbi.h — stable ABI boundary between qaiserve and postprocess .so
// plugins.
//
// Deliberately self-contained: no qai_forge includes, no nlohmann types in
// any signature. A plugin returns its result as a serialized JSON string
// (it may use nlohmann internally to build it) so the plugin build never
// needs to agree with qaiserve on a specific json library version.
//
// Every plugin and qaiserve itself are expected to be built by the same Docker 
// toolchain (see QAIServe/Dockerfile), so a C++ virtual-interface ABI is safe —
// there is no cross-compiler/cross-STL boundary to worry about.
//
// A plugin .so must export exactly these three extern "C" symbols:
//   postproc_abi::IPostprocess* CreatePostprocess();
//   void                        DestroyPostprocess(postproc_abi::IPostprocess*);
//   const char*                 PostprocAbiVersion();
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace postproc_abi {

// Bump on any breaking change to this header; PostprocPlugin
// rejects a plugin whose PostprocAbiVersion() doesn't match exactly.
inline const char* kAbiVersion = "1.0";

enum class DataType {
    FLOAT32,
    FLOAT16,
    INT32,
    INT16,
    INT8,
    UINT8,
    BOOL,
    BYTES,
};

// Number of DataType enumerators above. PostprocPlugin.cpp static_asserts
// its TensorDataType<->DataType lookup table against this so an enumerator
// added to one without a matching table row fails the build instead of
// silently misdecoding at runtime.
const size_t kDataTypeCount = 8;

// One tensor's shape/dtype expectations, one entry per output tensor in a
// given layout (see PluginDescription::layouts below). Each shape dimension is
// either an exact size or -1 (open wildcard, matches any size).
struct TensorExpectation {
    std::vector<int64_t>  shape;
    std::vector<DataType> accepted_dtypes;
};

// One request-time parameter a plugin accepts (query param on
// infer_postprocess) — advertised via discovery endpoints only, not
// enforced by the loader/registry.
struct ParameterSchema {
    std::string name;
    std::string type;
    std::string default_val;
    std::string description;
};

// Metadata-only tensor spec (name/shape/dtype/quant params), used for
// RequestConfig::input_specs/output_specs.
struct TensorSpec {
    std::string          name;
    std::vector<int64_t> shape;
    DataType             dtype = DataType::FLOAT32;
    float                quant_scale = 1.0f;
    int32_t              quant_zero_point = 0;
};

// A single raw output tensor at inference time — data + shape, no quant
// params (those travel separately via RequestConfig::output_specs, indexed
// positionally in the same order as this vector).
struct OutputTensor {
    std::string           name;
    std::vector<int64_t>  shape;
    DataType              dtype = DataType::FLOAT32;
    const uint8_t*        data = nullptr;
    size_t                data_len = 0;
};

struct RequestConfig {
    int  image_width = 0;
    int  image_height = 0;
    bool include_raw = false;
    int  layout_index = 0;
    std::vector<TensorSpec> input_specs;
    std::vector<TensorSpec> output_specs;
    std::string labels_path;
    std::map<std::string, std::string> extra;
};

// Static self-description of a plugin — name, description, supported tensor
// layouts, and accepted request parameters.
struct PluginDescription {
    std::string name;
    std::string description;

    // One entry per supported tensor arrangement; each entry is the ordered
    // list of per-tensor expectations for that arrangement. The server
    // tries each in order and uses the first the model's output_specs match.
    std::vector<std::vector<TensorExpectation>> layouts;

    // Advertised in discovery endpoints only.
    std::vector<ParameterSchema> parameters;
};

class IPostprocess {
public:
    virtual ~IPostprocess() = default;

    virtual PluginDescription pluginInfo() const = 0;

    // Returns a JSON-serialized result object (plugin dump()s its own
    // nlohmann::json internally). Must be safe to call concurrently from
    // multiple threads on the same instance — qaiserve creates one instance
    // per plugin and shares it across all requests.
    virtual std::string process(const std::vector<OutputTensor>& raw,
                                 const RequestConfig& cfg) const = 0;
};

}  // namespace postproc_abi

extern "C" {

postproc_abi::IPostprocess* CreatePostprocess();
void DestroyPostprocess(postproc_abi::IPostprocess* instance);
const char* PostprocAbiVersion();

}  // extern "C"

// Defines the extern "C" factory trio above for a concrete IPostprocess
// subclass. Invoke once at file scope in a plugin's .cpp:
//   POSTPROC_ABI_EXPORT(YourModelPostprocess)
#define POSTPROC_ABI_EXPORT(ClassName)                                    \
    extern "C" {                                                          \
    postproc_abi::IPostprocess* CreatePostprocess() {                     \
        return new ClassName();                                          \
    }                                                                     \
    void DestroyPostprocess(postproc_abi::IPostprocess* instance) {       \
        delete instance;                                                 \
    }                                                                     \
    const char* PostprocAbiVersion() {                                   \
        return postproc_abi::kAbiVersion;                                \
    }                                                                     \
    }  /* extern "C" */
