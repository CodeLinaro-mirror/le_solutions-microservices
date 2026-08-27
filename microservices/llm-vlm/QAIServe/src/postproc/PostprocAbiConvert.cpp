// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "postproc/PostprocAbiConvert.h"

#include <cstddef>

namespace postproc_convert {

namespace {

// Single source of truth for the TensorDataType <-> postproc_abi::DataType
// mapping. toAbi()/fromAbi() both scan this instead of maintaining two
// hand-written switches — a value missing from either enum's table row
// would otherwise silently decode as FLOAT32 (see the static_assert below,
// which catches a row/enumerator *count* mismatch at build time; it can't
// catch a row pointing at the wrong pair, so keep this table's order
// intentional, not just count-correct).
constexpr std::pair<TensorDataType, postproc_abi::DataType> kDtypeTable[] = {
    {TensorDataType::FLOAT32, postproc_abi::DataType::FLOAT32},
    {TensorDataType::FLOAT16, postproc_abi::DataType::FLOAT16},
    {TensorDataType::INT32,   postproc_abi::DataType::INT32},
    {TensorDataType::INT16,   postproc_abi::DataType::INT16},
    {TensorDataType::INT8,    postproc_abi::DataType::INT8},
    {TensorDataType::UINT8,   postproc_abi::DataType::UINT8},
    {TensorDataType::BOOL,    postproc_abi::DataType::BOOL},
    {TensorDataType::BYTES,   postproc_abi::DataType::BYTES},
};
static_assert(std::size(kDtypeTable) == postproc_abi::kDataTypeCount,
              "kDtypeTable must have exactly one row per postproc_abi::DataType enumerator");

}  // namespace

postproc_abi::DataType toAbi(TensorDataType dtype) {
    for (const auto& [server_dt, abi_dt] : kDtypeTable) {
        if (server_dt == dtype) return abi_dt;
    }
    return postproc_abi::DataType::FLOAT32;
}

TensorDataType fromAbi(postproc_abi::DataType dtype) {
    for (const auto& [server_dt, abi_dt] : kDtypeTable) {
        if (abi_dt == dtype) return server_dt;
    }
    return TensorDataType::FLOAT32;
}

postproc_abi::OutputTensor toAbi(const OutputTensor& t) {
    postproc_abi::OutputTensor out;
    out.name     = t.name;
    out.shape    = t.shape;
    out.dtype    = toAbi(t.dtype);
    out.data     = t.data.data();
    out.data_len = t.data.size();
    return out;
}

postproc_abi::TensorSpec toAbi(const ModelTensorSpec& spec) {
    postproc_abi::TensorSpec out;
    out.name             = spec.name;
    out.shape            = spec.shape;
    out.dtype            = toAbi(tensorDataTypeFromString(spec.dtype));
    out.quant_scale      = spec.quant_scale;
    out.quant_zero_point = spec.quant_zero_point;
    return out;
}

postproc_abi::RequestConfig toAbi(const PostprocConfig& cfg) {
    postproc_abi::RequestConfig out;
    out.image_width  = cfg.image_width;
    out.image_height = cfg.image_height;
    out.include_raw  = cfg.include_raw;
    out.layout_index = cfg.layout_index;

    out.input_specs.reserve(cfg.input_specs.size());
    for (const auto& s : cfg.input_specs) out.input_specs.push_back(toAbi(s));

    out.output_specs.reserve(cfg.output_specs.size());
    for (const auto& s : cfg.output_specs) out.output_specs.push_back(toAbi(s));

    out.labels_path = cfg.labels_path;
    out.extra       = cfg.extra;
    return out;
}

TensorExpectation fromAbi(const postproc_abi::TensorExpectation& expect) {
    TensorExpectation out;
    out.shape = expect.shape;

    out.accepted_dtypes.reserve(expect.accepted_dtypes.size());
    for (auto dt : expect.accepted_dtypes) out.accepted_dtypes.push_back(fromAbi(dt));

    return out;
}

ParameterSchema fromAbi(const postproc_abi::ParameterSchema& param) {
    return ParameterSchema{param.name, param.type, param.default_val, param.description};
}

}  // namespace postproc_convert
