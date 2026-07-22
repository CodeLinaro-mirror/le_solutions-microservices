// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "postproc_abi/PostprocAbi.h"

// ─────────────────────────────────────────────────────────────────────────────
// FaceDetPostprocess — decodes model's grid output (heatmap, bbox,
// landmark, optionally a heatmap-pool cross-check tensor) into face detections.
//
// Tensor roles are NOT assumed to be in a fixed output order: layouts()
// advertises several supported tensor arrangements (see the .cpp) with
// different tensor orderings and a 3- or 4-tensor variant. process() resolves
// which output index is heatmap/bbox/landmark/heatmap-pool from the tensor
// count and channel dimension (dimensions[3])
//
// Each tensor, once resolved:
//   heatmap  [*,*, 1]  — raw confidence logit, before sigmoid
//   bbox     [*,*, 4]  — (left,top,right,bottom) distance from cell centre
//                        to each edge, in stride units
//   landmark [*,*,10]  — 5 landmark offsets from cell centre, split layout:
//                        channels 0-4 = x, channels 5-9 = y
//
// Decode pipeline :
//   1. Raw box from bbox tensor, signed cell-relative edge distances.
//   2. If a heatmap-pool tensor is present, skip the cell when its (sigmoid)
//      score disagrees with the primary heatmap's.
//   3. Re-centre the box on the landmark centroid (landmarks are generally
//      the more reliable signal than the raw box regression).
//   4. Expand the shorter dimension so the final box is square.
//   5. Incremental per-box NMS: walk already-accepted boxes in grid scan
//      order; an IoU above threshold either replaces a lower-confidence
//      accepted box or rejects the candidate — no separate confidence sort
//      pass, unlike PostprocessUtils::greedyNMS.
// Reported detection score is ranged to 0-1.
// ─────────────────────────────────────────────────────────────────────────────
class FaceDetPostprocess : public postproc_abi::IPostprocess {
public:
    postproc_abi::PluginDescription pluginInfo() const override;

    std::string process(
        const std::vector<postproc_abi::OutputTensor>& raw,
        const postproc_abi::RequestConfig&              cfg) const override;
};
