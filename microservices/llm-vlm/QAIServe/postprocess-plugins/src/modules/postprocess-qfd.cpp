// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "modules/postprocess-qfd.h"
#include "postprocess-utils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>

#include <nlohmann/json.hpp>

using PostprocessUtils::Detection;
using postproc_abi::DataType;
using postproc_abi::OutputTensor;
using postproc_abi::RequestConfig;

namespace {

float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

const float kDefaultThreshold        = 0.70f;
const float kNMSIntersectionTreshold = 0.5f;
const float kBboxSizeTreshold        = 400.0f; // 20x20 px

} // namespace

std::string FaceDetPostprocess::process(
    const std::vector<OutputTensor>& raw,
    const RequestConfig&             cfg) const {

    const auto& outputs = raw;

    // Tensor role detection — Output index -> role varies by which of the
    // registered layouts entries matched (see pluginInfo() below), so resolve
    // roles from the tensor count and channel dimension rather than assuming
    // a fixed order.
    int scores_idx = 0, bboxes_idx = 1, landmarks_idx = 2, hm_pool_idx = -1;
    if (outputs.size() == 4) {
        // layout 2 — heatmap, heatmap-pool, landmark, bbox. The pool tensor
        // is used only as a cross-check below, never decoded directly.
        scores_idx    = 0;
        hm_pool_idx   = 1;
        landmarks_idx = 2;
        bboxes_idx    = 3;
    } else if (outputs[0].shape[3] == 4) {
        // layout 1 — bbox, landmark, heatmap
        bboxes_idx    = 0;
        landmarks_idx = 1;
        scores_idx    = 2;
    } else if (outputs[1].shape[3] == 4) {
        // layout 0 — heatmap, bbox, landmark
        scores_idx    = 0;
        bboxes_idx    = 1;
        landmarks_idx = 2;
    } else {
        // layout 3 — heatmap, landmark, bbox
        scores_idx    = 0;
        landmarks_idx = 1;
        bboxes_idx    = 2;
    }

    const OutputTensor& heatmap  = outputs[scores_idx];
    const OutputTensor& bbox     = outputs[bboxes_idx];
    const OutputTensor& landmark = outputs[landmarks_idx];
    const OutputTensor* hm_pool  = (hm_pool_idx >= 0) ? &outputs[hm_pool_idx] : nullptr;

    const float heatmap_scale   = cfg.output_specs[scores_idx].quant_scale;
    const int32_t heatmap_zp    = cfg.output_specs[scores_idx].quant_zero_point;
    const float bbox_scale      = cfg.output_specs[bboxes_idx].quant_scale;
    const int32_t bbox_zp       = cfg.output_specs[bboxes_idx].quant_zero_point;
    const float landmark_scale  = cfg.output_specs[landmarks_idx].quant_scale;
    const int32_t landmark_zp   = cfg.output_specs[landmarks_idx].quant_zero_point;
    const float hm_pool_scale   = (hm_pool_idx >= 0) ? cfg.output_specs[hm_pool_idx].quant_scale : 0.0f;
    const int32_t hm_pool_zp    = (hm_pool_idx >= 0) ? cfg.output_specs[hm_pool_idx].quant_zero_point : 0;

    // Grid dims come straight from the heatmap tensor shape [1, grid_h, grid_w, 1]
    // — shared by every tensor in a layout regardless of role ordering.
    const int64_t grid_h = heatmap.shape[1];
    const int64_t grid_w = heatmap.shape[2];

    // Model input spatial dims (NHWC: [1, H, W, C]) — used to derive the
    // paxel size (stride) from the actual model, rather than hardcoding it
    // for this one model. Falls back to a known 480x640 input
    // if input_specs is unexpectedly empty (should not happen for a
    // validated predictive model).
    int64_t input_h = 480;
    int64_t input_w = 640;
    if (!cfg.input_specs.empty() && cfg.input_specs[0].shape.size() >= 3) {
        input_h = cfg.input_specs[0].shape[1];
        input_w = cfg.input_specs[0].shape[2];
    }
    const float stride = static_cast<float>(input_w) / static_cast<float>(grid_w);

    // conf_threshold/iou_threshold are user-supplied and validated against
    // their valid ranges — confidence comes from sigmoid() so it can never
    // exceed [0,1], and IoU is a ratio of areas so it's always in [0,1]. An
    // out-of-range value falls back to the default instead of silently
    // rejecting every detection.
    const float conf_threshold = PostprocessUtils::getFloatInRange(
        cfg, "conf_threshold", kDefaultThreshold, 0.0f, 1.0f);
    const float iou_threshold = PostprocessUtils::getFloatInRange(
        cfg, "iou_threshold", kNMSIntersectionTreshold, 0.0f, 1.0f);
    const float min_box_area = kBboxSizeTreshold;

    std::vector<Detection> accepted;

    for (int64_t r = 0; r < grid_h; ++r) {
        for (int64_t c = 0; c < grid_w; ++c) {
            const size_t heatmap_idx = static_cast<size_t>(r * grid_w + c);
            const float confidence = sigmoid(
                PostprocessUtils::readFloat(heatmap, heatmap_idx, heatmap_scale, heatmap_zp));

            // hm_pool holds a local-max-pooled version of the heatmap (present
            // only in the 4-tensor layout). A true peak cell has pooled ==
            // raw score; if they differ, this cell isn't the local maximum of
            // its neighborhood — some other cell nearby scored higher — so
            // skip it here rather than letting NMS filter it out later.
            if (hm_pool) {
                const float pooled = sigmoid(
                    PostprocessUtils::readFloat(*hm_pool, heatmap_idx, hm_pool_scale, hm_pool_zp));
                if (confidence != pooled) continue;
            }

            if (confidence < conf_threshold) continue;

            // Grid-cell coordinates (not yet scaled to pixels — the reference
            // module folds the stride multiply into the box/landmark decode
            // below rather than computing a pixel-space cell centre first).
            const float cx = static_cast<float>(c);
            const float cy = static_cast<float>(r);

            const size_t bbox_base = heatmap_idx * 4;
            const float bbox_l = PostprocessUtils::readFloat(bbox, bbox_base + 0, bbox_scale, bbox_zp);
            const float bbox_t = PostprocessUtils::readFloat(bbox, bbox_base + 1, bbox_scale, bbox_zp);
            const float bbox_r = PostprocessUtils::readFloat(bbox, bbox_base + 2, bbox_scale, bbox_zp);
            const float bbox_b = PostprocessUtils::readFloat(bbox, bbox_base + 3, bbox_scale, bbox_zp);

            Detection cand;
            cand.score = confidence;
            cand.x1 = (cx - bbox_l) * stride;
            cand.y1 = (cy - bbox_t) * stride;
            cand.x2 = (cx + bbox_r) * stride;
            cand.y2 = (cy + bbox_b) * stride;

            const float box_area = (cand.x2 - cand.x1) * (cand.y2 - cand.y1);
            if (box_area < min_box_area) continue;

            // Landmarks — split channel layout (x0..x4 then y0..y4). Also used
            // to re-centre the box: the raw bbox decode above is a coarse
            // estimate, so the box gets re-centred on the landmark centroid.
            const size_t lm_base = heatmap_idx * 10;
            float ext_l = std::numeric_limits<float>::max();
            float ext_t = std::numeric_limits<float>::max();
            float ext_r = 0.0f;
            float ext_b = 0.0f;
            cand.landmarks.resize(5);
            for (int i = 0; i < 5; ++i) {
                const float ld_x = PostprocessUtils::readFloat(landmark, lm_base + i, landmark_scale, landmark_zp);
                const float ld_y = PostprocessUtils::readFloat(landmark, lm_base + i + 5, landmark_scale, landmark_zp);
                const float abs_x = (cx + ld_x) * stride;
                const float abs_y = (cy + ld_y) * stride;
                cand.landmarks[i].x = abs_x;
                cand.landmarks[i].y = abs_y;

                // Extents relative to the box origin, purely to compute the
                // re-centring translation below.
                const float local_x = abs_x - cand.x1;
                const float local_y = abs_y - cand.y1;
                ext_l = std::min(ext_l, local_x);
                ext_t = std::min(ext_t, local_y);
                ext_r = std::max(ext_r, local_x);
                ext_b = std::max(ext_b, local_y);
            }

            const float tx = ext_l + (ext_r - ext_l) / 2.0f - (cand.x2 - cand.x1) / 2.0f;
            const float ty = ext_t + (ext_b - ext_t) / 2.0f - (cand.y2 - cand.y1) / 2.0f;
            cand.x1 += tx; cand.x2 += tx;
            cand.y1 += ty; cand.y2 += ty;

            // Adjust bounding box dimensions to make it a square, expanding
            // the shorter dimension around the box centre.
            const float width  = cand.x2 - cand.x1;
            const float height = cand.y2 - cand.y1;
            if (width > height) {
                cand.y1 -= (width - height) / 2.0f;
                cand.y2 = cand.y1 + width;
            } else if (width < height) {
                cand.x1 -= (height - width) / 2.0f;
                cand.x2 = cand.x1 + height;
            }

            // Incremental per-box NMS — this checks already-accepted boxes 
            // in grid scan order and either replaces a lower-confidence 
            // overlap or rejects the candidate
            bool rejected = false;
            int  replace_idx = -1;
            for (size_t i = 0; i < accepted.size(); ++i) {
                if (PostprocessUtils::nmsIoU(cand, accepted[i]) <= iou_threshold) continue;
                if (cand.score > accepted[i].score) {
                    replace_idx = static_cast<int>(i);
                } else {
                    rejected = true;
                }
                break;
            }
            if (rejected) continue;
            if (replace_idx >= 0) {
                accepted.erase(accepted.begin() + replace_idx);
            }
            accepted.push_back(cand);
        }
    }

    // Scale from model-space pixels to the caller's original image dims if
    // provided; otherwise leave coordinates in model-space (input_w x input_h).
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    if (cfg.image_width > 0 && cfg.image_height > 0) {
        scale_x = static_cast<float>(cfg.image_width) / static_cast<float>(input_w);
        scale_y = static_cast<float>(cfg.image_height) / static_cast<float>(input_h);
    }

    nlohmann::json detections_json = nlohmann::json::array();
    for (const auto& d : accepted) {
        nlohmann::json landmarks_json = nlohmann::json::array();
        for (const auto& lm : d.landmarks) {
            landmarks_json.push_back({lm.x * scale_x, lm.y * scale_y});
        }
        detections_json.push_back({
            {"score", d.score},
            {"bbox", {d.x1 * scale_x, d.y1 * scale_y, d.x2 * scale_x, d.y2 * scale_y}},
            {"landmarks", landmarks_json},
        });
    }

    nlohmann::json result;
    result["detections"]   = detections_json;
    result["image_width"]  = (cfg.image_width > 0) ? cfg.image_width : static_cast<int>(input_w);
    result["image_height"] = (cfg.image_height > 0) ? cfg.image_height : static_cast<int>(input_h);
    return result.dump();
}

postproc_abi::PluginDescription FaceDetPostprocess::pluginInfo() const {
    postproc_abi::PluginDescription d;
    d.name = "face_detect_qfd";
    d.description =
        "Returns a JSON object with: detections — array of accepted faces after "
        "confidence filtering, min-box-area filtering, and NMS, each with: "
        "score — confidence in 0-1; bbox — [x1,y1,x2,y2] in the caller's "
        "image-space px; landmarks — array of 5 {x,y} points in the caller's "
        "image-space px. image_width, image_height — dims the bbox/landmark "
        "coords are scaled to (cfg.image_width/height if provided, otherwise "
        "model input dims). Input tensor order varies by layouts below, "
        "resolved at runtime from tensor count and channel dims";
    d.layouts = {
        {   // layout 0
            {{1,60,80, 1}, {DataType::UINT8, DataType::FLOAT16, DataType::FLOAT32}},
            {{1,60,80, 4}, {DataType::UINT8, DataType::FLOAT16, DataType::FLOAT32}},
            {{1,60,80,10}, {DataType::UINT8, DataType::FLOAT16, DataType::FLOAT32}},
        },
        {   // layout 1
            {{1,60,80, 4}, {DataType::UINT8, DataType::FLOAT16, DataType::FLOAT32}},
            {{1,60,80,10}, {DataType::UINT8, DataType::FLOAT16, DataType::FLOAT32}},
            {{1,60,80, 1}, {DataType::UINT8, DataType::FLOAT16, DataType::FLOAT32}},
        },
        {   // layout 2
            {{1,60,80, 1}, {DataType::UINT8, DataType::FLOAT16, DataType::FLOAT32}},
            {{1,60,80, 1}, {DataType::UINT8, DataType::FLOAT16, DataType::FLOAT32}},
            {{1,60,80,10}, {DataType::UINT8, DataType::FLOAT16, DataType::FLOAT32}},
            {{1,60,80, 4}, {DataType::UINT8, DataType::FLOAT16, DataType::FLOAT32}},
        },
        {   // layout 3
            {{1,120,160, 1}, {DataType::UINT8, DataType::FLOAT16, DataType::FLOAT32}},
            {{1,120,160,10}, {DataType::UINT8, DataType::FLOAT16, DataType::FLOAT32}},
            {{1,120,160, 4}, {DataType::UINT8, DataType::FLOAT16, DataType::FLOAT32}},
        },
    };
    d.parameters = {
        {"conf_threshold", "float", "0.70", "Minimum detection confidence"},
        {"iou_threshold",  "float", "0.5",  "NMS IoU overlap threshold"},
    };
    return d;
}

// ── bottom of postprocess-qfd.cpp — ABI factory trio ───────────────────────
POSTPROC_ABI_EXPORT(FaceDetPostprocess)
