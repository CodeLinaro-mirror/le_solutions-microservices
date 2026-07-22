// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/managers/ModelConfigManager.h"

#include <map>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// PostprocConfig — per-request context passed to PostprocPlugin::process()
//
// Built by InferController from HTTP query parameters plus the model's tensor
// specs, and passed into every process() call — this is the only argument a
// postprocess needs (besides the raw tensors), so PostprocPlugin::process()
// takes no separate ModelConfig parameter. Common fields are typed and parsed
// centrally. layout_index carries which layouts[] entry matched during
// compatibility checking — the postprocess branches on it to select the
// correct decode path. Model-specific thresholds travel in `extra` as raw
// strings.
// ─────────────────────────────────────────────────────────────────────────────
struct PostprocConfig {
    // Parsed centrally by InferController from HTTP query params
    int  image_width  = 0;      // 0 = not provided; fall back to model-space coords
    int  image_height = 0;
    bool include_raw  = false;  // if true, raw OIP outputs[] appended to response
    int  layout_index = 0;      // which layouts[] entry matched — postprocess branches on this

    // Model tensor specs from metadata.json (ModelConfig::input_specs/output_specs) —
    // output_specs carries quant_scale/quant_zero_point per tensor, needed to
    // dequantize UINT8/INT8 outputs. Copied in by InferController so postprocess
    // never need direct access to ModelConfig or ModelConfigManager.
    std::vector<ModelTensorSpec> input_specs;
    std::vector<ModelTensorSpec> output_specs;

    // Absolute path to the labels file, resolved by InferController from
    // ModelConfig::config_file's directory — empty if config_file is empty.
    // Postprocess that needs class names read this file via
    // PostprocessUtils::loadLabelsFile(); the file may be plain
    // text (one name per line) or another format a given postprocessor
    // chooses to parse — see PostprocessUtils.h.
    std::string labels_path;

    // Model-specific query params — each postprocessor reads what it needs
    std::map<std::string, std::string> extra;
    // e.g. { "conf_threshold": "0.6", "iou_threshold": "0.45" }
};
