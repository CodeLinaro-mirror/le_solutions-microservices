// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "postproc_abi/PostprocAbi.h"

// ─────────────────────────────────────────────────────────────────────────────
// PersonDetPostprocess — decodes a person/pose detection model's 4-tensor
// grid output (scores, bboxes, landmarks, landmark-scores) into per-class
// object detections with optional named keypoints.
//
// This model's tensor order is fixed (no dynamic role detection):
//   0: scores    [1,120,160, 3]  UINT8|FP16|FP32 — per-class confidence, one grid of
//                                scores per class, already post-activation
//   1: bboxes    [1,120,160,12]  UINT8|FP16|FP32 — per-class (left,top,right,bottom)
//                                cell-relative distances, 4 floats per class
//   2: landmarks [1,120,160,34]  UINT8|FP16|FP32 — 17 keypoints per grid cell, split
//                                layout: 17 x-offsets then 17 y-offsets
//   3: lmkscores [1,120,160,17]  UINT8|FP16|FP32 — per-keypoint confidence
//
// Class names are NOT hardcoded: this port loads them from labels.txt
// alongside the model's file (one name per line, by position) via
// PostprocessUtils::loadLabelsFile(cfg.labels_path, /*required=*/true) —
// required, since this postprocess's whole output is meaningless without
// names (the request fails with an HTTP 500 if the file is missing or
// empty). Keypoint names, unlike class names, have no plain-text file
// schema for the nested class -> keypoint-id -> name mapping, so they still
// travel as a per-request query parameter (forwarded into
// RequestConfig::extra by InferController):
//   ?landmarks=[{"id":0,"landmarks_names":[{"id":0,"name":"nose"}, ...]}, ...]
//     A class with no entry, or a keypoint id with no name, is simply
//     omitted from that detection's landmarks[] — the box is still emitted.
// `landmarks` is optional; omitting it simply yields no landmarks on any
// detection.
//
// NMS is per-class: incremental,walks already-accepted boxes in grid scan order,
// and an IoU above threshold against a box of the SAME class either replaces a
// lower-confidence accepted box or rejects the candidate. Boxes of different
// classes never suppress each other.
// ─────────────────────────────────────────────────────────────────────────────
class PersonDetPostprocess : public postproc_abi::IPostprocess {
public:
    postproc_abi::PluginDescription pluginInfo() const override;

    std::string process(
        const std::vector<postproc_abi::OutputTensor>& raw,
        const postproc_abi::RequestConfig&              cfg) const override;
};
