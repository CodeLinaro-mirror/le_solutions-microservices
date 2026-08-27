// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "postproc_abi/PostprocAbi.h"

// ─────────────────────────────────────────────────────────────────────────────
// HrnetPostprocess — decodes a top-down, single-pose HRNet heatmap output
// into named keypoints (with sub-pixel refinement) and optional skeleton
// links.
//
// One output tensor [1, grid_h, grid_w, n_keypoints] UINT8|FP16|FP32 — grid_h/grid_w
// and n_keypoints are open wildcards here. HRNet is a top-down single-pose estimator: the model
// input is assumed to already be a crop containing exactly one person, so
// Process() always emits at most one pose (never per-cell multi-instance
// detection, and no NMS).
//
// Decode, per keypoint channel:
//   1. Scan every spatial position's value for this channel and keep the
//      argmax location (Module::TensorCompareValues) — the heatmap's raw
//      value at that location IS the keypoint confidence, no sigmoid ( HRNet heatmaps
//      are trained directly against a Gaussian target, not a logit).
//   2. Sub-pixel refine by comparing the immediate horizontal/vertical
//      neighbors of the argmax cell: whichever neighbor is larger nudges the
//      coordinate a quarter-cell in that direction (dx/dy in {-1,0,1} * 0.25).
//   3. Scale from grid space to the model's input pixel space, then clamp to
//      the input frame.
// Overall pose confidence is the mean of all per-keypoint confidences; the
// pose is only emitted if that mean clears conf_threshold.
//
// Class names are NOT hardcoded: this port loads them from labels.txt
// alongside the model's file (one name per line, by position) via
// PostprocessUtils::loadLabelsFile(cfg.labels_path, /*required=*/false) —
// unlike the classification/detection/segmentation postprocesses, this is
// OPTIONAL: a missing/unreadable/empty file yields "unknown" for
// every keypoint rather than failing the request, since pose coordinates
// remain useful even unnamed. Skeleton links are similarly not hardcoded, but have no
// plain-text file schema, so they still travel as a per-request query
// parameter (forwarded into RequestConfig::extra by InferController):
//   ?connections=[{"id":0,"connection":1}, ...]
//     Each entry pairs two keypoint indices into one skeleton link in the
//     output; an out-of-range index is skipped.
// `connections` is optional; omitting it simply yields no links[].
// ─────────────────────────────────────────────────────────────────────────────
class HrnetPostprocess : public postproc_abi::IPostprocess {
public:
    postproc_abi::PluginDescription pluginInfo() const override;

    std::string process(
        const std::vector<postproc_abi::OutputTensor>& raw,
        const postproc_abi::RequestConfig&              cfg) const override;
};
