// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "postproc_abi/PostprocAbi.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// PostprocessUtils — shared stateless helpers. Built against postproc_abi types
// only (no qai_forge dependency) since this is linked into each plugin .so.
// ─────────────────────────────────────────────────────────────────────────────
namespace PostprocessUtils {

// dtype-aware element read — call this for every tensor element.
// UINT8/INT8 -> dequant   FP16 -> fp16ToFloat   FP32 -> direct cast
float readFloat(const postproc_abi::OutputTensor& t, size_t idx,
                 float quant_scale, int32_t quant_zero_point);

// Pull a typed param from RequestConfig::extra with a fallback default.
// Fails closed: missing or malformed input returns default_val, never throws.
float getFloat(const postproc_abi::RequestConfig& cfg, const std::string& key, float default_val);

// Same as getFloat, but additionally rejects an out-of-[lo,hi] value by
// falling back to default_val, rather than silently clamping it to the
// boundary. Used for user-supplied thresholds/ratios that have a natural
// valid range (e.g. confidence and IoU are always in [0,1]).
float getFloatInRange(const postproc_abi::RequestConfig& cfg, const std::string& key,
                       float default_val, float lo, float hi);

// Loads a plain-text labels file — one label name per line (blank lines
// skipped), class_idx assigned by position (0-indexed) among non-blank
// lines. `labels_path` is the fully resolved path built by InferController
// from the model's config_file directory (RequestConfig::labels_path) —
// labels.txt is expected to sit alongside the model file.
// If `required` is true, throws std::runtime_error when labels_path is
// empty, the file is missing, or the file is empty — the caller's
// postprocess has no meaningful output without class names. If
// `required` is false, any of those conditions instead yields an empty map
// (every class then reads as "unknown").
std::map<int, std::string> loadLabelsFile(const std::string& labels_path, bool required);

// Shared detection representation — reused across postprocess modules 
// instead of each module declaring its own local copy. A landmark 
// with no name is still valid — name is simply left empty.
struct Keypoint {
    std::string name;
    float x = 0.0f, y = 0.0f;
    float score = 0.0f;
};

struct Detection {
    int         class_idx = 0;
    std::string name;
    float       score = 0.0f;
    float       x1 = 0.0f, y1 = 0.0f, x2 = 0.0f, y2 = 0.0f;
    std::vector<Keypoint> landmarks;
};

// IoU between two axis-aligned boxes, used to decide NMS overlap — same
// formula as Module::IntersectionScore() in the reference GStreamer modules.
float nmsIoU(const Detection& a, const Detection& b);

} // namespace PostprocessUtils
