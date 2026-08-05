// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "qai_forge/adapters/VisionPreprocessAdapter.h"

// ─────────────────────────────────────────────────────────────────────────────
// DefaultVisionPreprocessAdapter — Generic VLM patch preprocessing
//
// Implements the standard reshape + transpose algorithm for VLM models that
// do not have a specific preprocessing requirement.
//
// Patch ordering: row-major over (grid_h, grid_w), with merge blocks iterated
// as (gh_m, gw_m, ms_h, ms_w). This produces a patch index:
//   l = (gh_m * merge_size + ms_h) * grid_w + (gw_m * merge_size + ms_w)
//
// Feature ordering within each patch:
//   d = c * (temporal_patch_size * patch_size²) + t * patch_size² + ph * patch_size + pw
//
// Used as the fallback for any VLM model not matched by a specific adapter.
// ─────────────────────────────────────────────────────────────────────────────
class DefaultVisionPreprocessAdapter : public VisionPreprocessAdapter {
public:
    std::vector<float> preprocessPatches(
        const std::vector<float>& chw_data,
        int width,
        int height,
        const VisionPreprocessConfig& config) const override;

    std::string getName() const override { return "DefaultVisionPreprocess"; }
};
