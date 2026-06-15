// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/adapters/VisionPreprocessAdapter.h"

// ─────────────────────────────────────────────────────────────────────────────
// QwenVisionPreprocessAdapter — Qwen VLM patch preprocessing
//
// Implements the Qwen2-VL / Qwen2.5-VL / Qwen3-VL specific patch ordering
// algorithm. Used for all Qwen vision-language models.
//
// Patch ordering: block-major over (bh, bw, ms_h, ms_w), where:
//   bh   = block row index    (0 .. grid_h/merge_size - 1)
//   bw   = block column index (0 .. grid_w/merge_size - 1)
//   ms_h = merge row within block (0 .. merge_size - 1)
//   ms_w = merge col within block (0 .. merge_size - 1)
//
// Patch index encoding:
//   i = bh * (merge_size² * BW) + bw * merge_size² + ms_h * merge_size + ms_w
//   where BW = (W / patch_size) / merge_size  (blocks per row)
//
// Source pixel index for patch i, pixel pix in the CHW plane:
//   row = (bh * merge_size + ms_h) * patch_size + (pix / patch_size)
//   col = (bw * merge_size + ms_w) * patch_size + (pix % patch_size)
//   idx_src = row * W + col
//
// Feature ordering within each patch (same as DefaultVisionPreprocessAdapter):
//   d = c * (temporal_patch_size * patch_size²) + t * patch_size² + pix
//
// This ordering groups spatially adjacent merge-block patches together,
// which is required by the Qwen image encoder's attention mechanism.
// ─────────────────────────────────────────────────────────────────────────────
class QwenVisionPreprocessAdapter : public VisionPreprocessAdapter {
public:
    std::vector<float> preprocessPatches(
        const std::vector<float>& chw_data,
        int width,
        int height,
        const VisionPreprocessConfig& config) const override;

    std::string getName() const override { return "QwenVisionPreprocess"; }
};
