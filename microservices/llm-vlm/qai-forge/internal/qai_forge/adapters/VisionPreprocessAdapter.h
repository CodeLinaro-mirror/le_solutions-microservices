// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <string>
#include <vector>

// Forward-declare VisionPreprocessConfig to avoid pulling in the full ImageUtils header.
// Implementations that call preprocessPatches() must include ImageUtils.h.
struct VisionPreprocessConfig;

// ─────────────────────────────────────────────────────────────────────────────
// VisionPreprocessAdapter — Abstract interface for VLM patch preprocessing
//
// Encapsulates the model-specific step of converting a normalized CHW float
// image into the (L, D) patch tensor expected by the GenIE image encoder node.
//
// The common preprocessing pipeline (decode → letterbox → floor → normalize →
// CHW) is handled by ImageUtils::preprocessImage() and is model-agnostic.
// Only the final reshape + transpose step (patch ordering) is model-specific
// and is delegated to the appropriate adapter.
//
// Patch tensor dimensions:
//   L = (W / patch_size) * (H / patch_size)   — number of patches
//   D = C * temporal_patch_size * patch_size²  — floats per patch
//
// Resolved by VisionPreprocessAdapterFactory::getAdapter(model_id).
// ─────────────────────────────────────────────────────────────────────────────
class VisionPreprocessAdapter {
public:
    virtual ~VisionPreprocessAdapter() = default;

    /**
     * Convert a normalized CHW float image into the (L, D) patch tensor.
     *
     * @param chw_data  Normalized float array in [C, H, W] planar order.
     *                  Layout: R plane [0..H*W-1], G plane [H*W..2*H*W-1],
     *                          B plane [2*H*W..3*H*W-1].
     * @param width     Image width after preprocessing (multiple of patch_size * merge_size).
     * @param height    Image height after preprocessing (multiple of patch_size * merge_size).
     * @param config    Model-specific preprocessing parameters (patch_size, merge_size,
     *                  temporal_patch_size).
     * @return          Flat float array of shape (L * D) in row-major order.
     */
    virtual std::vector<float> preprocessPatches(
        const std::vector<float>& chw_data,
        int width,
        int height,
        const VisionPreprocessConfig& config) const = 0;

    /**
     * Returns the adapter name for logging and diagnostics.
     */
    virtual std::string getName() const = 0;
};
