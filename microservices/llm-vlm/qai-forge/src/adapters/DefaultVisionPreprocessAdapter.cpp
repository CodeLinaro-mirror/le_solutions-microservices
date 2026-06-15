// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/adapters/DefaultVisionPreprocessAdapter.h"
#include "qai_forge/utils/ImageUtils.h"

// ─────────────────────────────────────────────────────────────────────────────
// DefaultVisionPreprocessAdapter::preprocessPatches
//
// Standard reshape + transpose algorithm for VLM models without a specific
// preprocessing requirement.
//
// Algorithm (matches Python reference):
//   patches = frames.reshape(grid_t, temporal_patch_size, C,
//                            grid_h//merge_size, merge_size, patch_size,
//                            grid_w//merge_size, merge_size, patch_size)
//   patches = patches.transpose(0, 3, 6, 4, 7, 2, 1, 5, 8)
//   flat    = patches.reshape(L, D)
//
// Patch index l = gt*(grid_h*grid_w) + (gh_m*M+ms_h)*grid_w + (gw_m*M+ms_w)
// Feature index d = c*(T*P²) + t*P² + ph*P + pw
// ─────────────────────────────────────────────────────────────────────────────
std::vector<float> DefaultVisionPreprocessAdapter::preprocessPatches(
    const std::vector<float>& chw_data,
    int width,
    int height,
    const VisionPreprocessConfig& config) const {

    const int C = 3;
    const int W = width;
    const int H = height;
    const int T = config.temporal_patch_size;
    const int P = config.patch_size;
    const int M = config.merge_size;

    // ── Build temporal frames (duplicate static image for all T frames) ────────
    // frames[t, c, h, w] = chw_data[c, h, w]  for all t
    std::vector<float> frames(T * C * H * W);
    for (int t = 0; t < T; ++t) {
        std::copy(chw_data.begin(), chw_data.end(), frames.begin() + t * C * H * W);
    }

    // ── Compute grid dimensions ────────────────────────────────────────────────
    const int grid_t     = 1;           // single temporal group for static images
    const int grid_h     = H / P;
    const int grid_w     = W / P;
    const int gh_m_count = grid_h / M;  // merge blocks per column
    const int gw_m_count = grid_w / M;  // merge blocks per row
    const int L          = grid_t * grid_h * grid_w;
    const int D          = C * T * P * P;

    // ── Reshape + transpose to (L, D) ─────────────────────────────────────────
    std::vector<float> flat(L * D);

    for (int gt = 0; gt < grid_t; ++gt) {
        for (int gh_m = 0; gh_m < gh_m_count; ++gh_m) {
            for (int gw_m = 0; gw_m < gw_m_count; ++gw_m) {
                for (int ms_h = 0; ms_h < M; ++ms_h) {
                    for (int ms_w = 0; ms_w < M; ++ms_w) {
                        // Patch index in row-major order
                        const int gh = gh_m * M + ms_h;
                        const int gw = gw_m * M + ms_w;
                        const int l  = gt * (grid_h * grid_w) + gh * grid_w + gw;

                        for (int c = 0; c < C; ++c) {
                            for (int t = 0; t < T; ++t) {
                                for (int ph = 0; ph < P; ++ph) {
                                    for (int pw = 0; pw < P; ++pw) {
                                        // Feature index within patch
                                        const int d = c * (T * P * P)
                                                    + t * (P * P)
                                                    + ph * P + pw;

                                        // Source pixel in frames[src_t, c, h, w]
                                        const int h     = gh_m * M * P + ms_h * P + ph;
                                        const int w     = gw_m * M * P + ms_w * P + pw;
                                        const int src_t = gt * T + t;
                                        const int src   = src_t * C * H * W
                                                        + c * H * W
                                                        + h * W + w;

                                        flat[l * D + d] = frames[src];
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    return flat;
}
