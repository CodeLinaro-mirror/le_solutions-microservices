// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/adapters/QwenVisionPreprocessAdapter.h"
#include "qai_forge/utils/ImageUtils.h"

// ─────────────────────────────────────────────────────────────────────────────
// QwenVisionPreprocessAdapter::preprocessPatches
//
// Implements the Qwen VLM patch ordering algorithm (Qwen2-VL, Qwen2.5-VL,
// Qwen3-VL). This is a direct translation of the reference C implementation:
//
//   for (i = 0; i < L; ++i) {
//     for (pix = 0; pix < P*P; ++pix) {
//       idx_src = ((((i/(M*M*BW))*M + (i/M)%M)*P + pix/P)) * W
//               + ((((i/(M*M))%BW)*M + i%M)*P + pix%P);
//       for (c = 0; c < C; ++c)
//         for (t = 0; t < T; ++t)
//           out[i*D + c*T*P*P + pix + t*P*P] = src_c[idx_src];
//     }
//   }
//
// Patch index encoding (block-major order):
//   i = bh*(M²*BW) + bw*M² + ms_h*M + ms_w
//   where:
//     bh   = i / (M*M*BW)          — block row
//     bw   = (i / (M*M)) % BW      — block column
//     ms_h = (i / M) % M           — merge row within block
//     ms_w = i % M                 — merge column within block
//     BW   = (W/P) / M             — blocks per row
//
// Source pixel coordinates for patch i, pixel pix:
//   row = (bh*M + ms_h)*P + (pix/P)
//   col = (bw*M + ms_w)*P + (pix%P)
//   idx_src = row*W + col
//
// This groups spatially adjacent merge-block patches together, which is
// required by the Qwen image encoder's windowed attention mechanism.
// ─────────────────────────────────────────────────────────────────────────────
std::vector<float> QwenVisionPreprocessAdapter::preprocessPatches(
    const std::vector<float>& chw_data,
    int width,
    int height,
    const VisionPreprocessConfig& config) const {

    const size_t C = 3;
    const size_t W = static_cast<size_t>(width);
    const size_t H = static_cast<size_t>(height);
    const size_t P = static_cast<size_t>(config.patch_size);
    const size_t M = static_cast<size_t>(config.merge_size);
    const size_t T = static_cast<size_t>(config.temporal_patch_size);

    // Number of patches and features per patch
    const size_t Gw = W / P;          // patches per row
    const size_t BW = Gw / M;         // blocks per row
    const size_t L  = (W / P) * (H / P);
    const size_t D  = C * T * P * P;

    // chw_data layout: [C, H, W] planar
    //   R plane: chw_data[0 .. H*W-1]
    //   G plane: chw_data[H*W .. 2*H*W-1]
    //   B plane: chw_data[2*H*W .. 3*H*W-1]
    const float* plane[3] = {
        chw_data.data(),
        chw_data.data() + H * W,
        chw_data.data() + 2 * H * W
    };

    std::vector<float> out(L * D);

    for (size_t i = 0; i < L; ++i) {
        for (size_t pix = 0; pix < P * P; ++pix) {
            // Decode patch index i into block-major coordinates and compute
            // the source pixel index in the CHW plane.
            //
            // Row in image: (bh*M + ms_h)*P + (pix/P)
            //   bh   = i / (M*M*BW)
            //   ms_h = (i / M) % M
            //
            // Col in image: (bw*M + ms_w)*P + (pix%P)
            //   bw   = (i / (M*M)) % BW
            //   ms_w = i % M
            const size_t idx_src =
                (((i / (M * M * BW)) * M + ((i / M) % M)) * P + (pix / P)) * W +
                ((((i / (M * M)) % BW) * M + (i % M)) * P + (pix % P));

            for (size_t c = 0; c < C; ++c) {
                const float pixel = plane[c][idx_src];

                // Duplicate the same pixel for all temporal frames
                for (size_t t = 0; t < T; ++t) {
                    out[i * D + c * T * P * P + pix + t * P * P] = pixel;
                }
            }
        }
    }

    return out;
}
