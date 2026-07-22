// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "postproc_abi/PostprocAbi.h"

// ─────────────────────────────────────────────────────────────────────────────
// Yolov8Postprocess — decodes a YOLOv8 object-detection export's output
// tensor(s) into per-class object detections.
//
// YOLOv8 exports come in three different tensor arrangements depending on
// the export tool/options — resolved at runtime from raw.size():
//   - 1 tensor  (mono-block):  [1, 4+n_classes, n_paxels] UINT8|FP16|FP32 — box channels
//                               0-3 (cx,cy,w,h) then n_classes score channels,
//                               all channel-major over n_paxels.
//   - 2 tensors (dual-block):  tensor0 [1,4,n_paxels] boxes (cx,cy,w,h),
//                               tensor1 [1,n_classes,n_paxels] scores —
//                               same channel-major layout, split across
//                               two tensors instead of one.
//   - 3 tensors (triple-block): tensor0 [1,n_paxels,4] boxes, already
//                               (left,top,right,bottom) absolute per paxel;
//                               tensor1 [1,n_paxels] scores; tensor2
//                               [1,n_paxels] class indices (as float) — this
//                               is the "decoded" export variant where argmax
//                               over classes already happened upstream.
// n_paxels and n_classes are read from tensor shapes at runtime (not
// hardcoded) — layouts()'s entries below use -1 wildcards on those
// dimensions so any YOLOv8 export size is accepted.
//
// For mono-/dual-block, the best-scoring class per paxel is found by
// scanning all class channels; for triple-block the class index is read directly.
//
// Class names come from labels.txt alongside the model's file (one
// name per line, by position) — required; the request fails if the file
// is missing or empty (see PostprocessUtils::loadLabelsFile). Fallback: a class
// index with no entry is reported with name "unknown" rather than being dropped.
//
// NMS is per-class, walks already-accepted boxes in scan order, and an IoU
// above threshold against a box of the SAME name either replaces a lower-confidence
// accepted box or rejects the candidate.
// ─────────────────────────────────────────────────────────────────────────────
class Yolov8Postprocess : public postproc_abi::IPostprocess {
public:
    postproc_abi::PluginDescription pluginInfo() const override;

    std::string process(
        const std::vector<postproc_abi::OutputTensor>& raw,
        const postproc_abi::RequestConfig&              cfg) const override;
};
