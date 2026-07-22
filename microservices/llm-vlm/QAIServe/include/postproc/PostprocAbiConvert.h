// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "postproc/PostprocConfig.h"
#include "postproc/PostprocRegistry.h"
#include "postproc_abi/PostprocAbi.h"
#include "qai_forge/dto/TensorDTOs.h"

// ─────────────────────────────────────────────────────────────────────────────
// postproc_convert — server-side types <-> postproc_abi types.
//
// Shared by PostprocPlugin::process() (server -> ABI, per request) and
// PostprocRegistry::loadOne() (ABI -> server, once per plugin at
// startup) — kept in one place so the TensorDataType <-> postproc_abi::DataType
// table exists exactly once, not duplicated across the two call sites.
// ─────────────────────────────────────────────────────────────────────────────
namespace postproc_convert {

postproc_abi::DataType toAbi(TensorDataType dtype);
TensorDataType fromAbi(postproc_abi::DataType dtype);

// View only — the returned OutputTensor::data points into `t.data`, so it is
// valid only as long as `t` outlives it.
postproc_abi::OutputTensor toAbi(const OutputTensor& t);

postproc_abi::TensorSpec toAbi(const ModelTensorSpec& spec);
postproc_abi::RequestConfig toAbi(const PostprocConfig& cfg);

TensorExpectation fromAbi(const postproc_abi::TensorExpectation& expect);
ParameterSchema fromAbi(const postproc_abi::ParameterSchema& param);

}  // namespace postproc_convert
