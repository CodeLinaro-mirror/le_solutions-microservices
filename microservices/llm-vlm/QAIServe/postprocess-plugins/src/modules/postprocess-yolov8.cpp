// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "modules/postprocess-yolov8.h"
#include "postprocess-utils.h"

#include <algorithm>
#include <map>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

using PostprocessUtils::Detection;
using postproc_abi::DataType;
using postproc_abi::OutputTensor;
using postproc_abi::RequestConfig;

namespace {

std::string labelFor(int class_idx, const std::map<int, std::string>& labels) {
    auto it = labels.find(class_idx);
    return (it != labels.end()) ? it->second : "unknown";
}

const float kDefaultThreshold        = 0.70f;
const float kNMSIntersectionTreshold = 0.5f;

// Incremental NMS keyed on resolved label name —
// walks already-accepted boxes in scan order; an IoU above threshold against
// a box of the SAME name either replaces a lower-confidence accepted box or
// rejects the candidate. If no `labels` param is supplied every detection is
// named "unknown" and NMS becomes global across classes.
void acceptWithNMS(Detection&& cand, std::vector<Detection>& accepted, float iou_threshold) {
    for (size_t i = 0; i < accepted.size(); ++i) {
        if (accepted[i].name != cand.name) continue;
        if (PostprocessUtils::nmsIoU(cand, accepted[i]) <= iou_threshold) continue;
        if (cand.score > accepted[i].score) {
            accepted.erase(accepted.begin() + i);
            accepted.push_back(std::move(cand));
        }
        return; // either replaced or rejected — reference stops at first overlap
    }
    accepted.push_back(std::move(cand));
}

// Mono-block (1 tensor) — [1, 4+n_classes, n_paxels] FP32, channel-major:
// channels 0-3 are (cx,cy,w,h) each n_paxels contiguous, channels 4.. are
// per-class scores each n_paxels contiguous. Boxes are already absolute
// pixel coordinates in the model's input frame (no stride/grid decode).
std::vector<Detection> decodeMonoblock(
    const OutputTensor& tensor, const RequestConfig& cfg,
    const std::map<int, std::string>& labels,
    float conf_threshold, float iou_threshold,
    int64_t input_w, int64_t input_h) {

    const float scale   = cfg.output_specs[0].quant_scale;
    const int32_t zp     = cfg.output_specs[0].quant_zero_point;
    const int64_t n_paxels  = tensor.shape[2];
    const int64_t n_classes = tensor.shape[1] - 4;
    const int64_t score_base = 4 * n_paxels;

    std::vector<Detection> accepted;
    for (int64_t idx = 0; idx < n_paxels; ++idx) {
        int64_t best = idx;
        for (int64_t num = idx + n_paxels; num < n_classes * n_paxels; num += n_paxels) {
            if (PostprocessUtils::readFloat(tensor, score_base + num, scale, zp) >
                PostprocessUtils::readFloat(tensor, score_base + best, scale, zp)) {
                best = num;
            }
        }
        const int class_idx = static_cast<int>(best / n_paxels);
        const float confidence = PostprocessUtils::readFloat(tensor, score_base + best, scale, zp);
        if (confidence < conf_threshold) continue;

        const float cx = PostprocessUtils::readFloat(tensor, idx, scale, zp);
        const float cy = PostprocessUtils::readFloat(tensor, idx + n_paxels, scale, zp);
        const float w  = PostprocessUtils::readFloat(tensor, idx + 2 * n_paxels, scale, zp);
        const float h  = PostprocessUtils::readFloat(tensor, idx + 3 * n_paxels, scale, zp);

        Detection cand;
        cand.class_idx = class_idx;
        cand.name       = labelFor(class_idx, labels);
        cand.score      = confidence;
        cand.x1 = std::max(cx - w / 2.0f, 0.0f);
        cand.y1 = std::max(cy - h / 2.0f, 0.0f);
        cand.x2 = std::min(cand.x1 + w, static_cast<float>(input_w));
        cand.y2 = std::min(cand.y1 + h, static_cast<float>(input_h));

        acceptWithNMS(std::move(cand), accepted, iou_threshold);
    }
    return accepted;
}

// Dual-block (2 tensors) — tensor0 [1,4,n_paxels] boxes (cx,cy,w,h) channel-
// major, tensor1 [1,n_classes,n_paxels] scores channel-major. Same decode as
// mono-block, just split across two tensors instead of one.
std::vector<Detection> decodeDualblock(
    const OutputTensor& boxes_t, const OutputTensor& scores_t, const RequestConfig& cfg,
    const std::map<int, std::string>& labels,
    float conf_threshold, float iou_threshold,
    int64_t input_w, int64_t input_h) {

    const float boxes_scale  = cfg.output_specs[0].quant_scale;
    const int32_t boxes_zp    = cfg.output_specs[0].quant_zero_point;
    const float scores_scale = cfg.output_specs[1].quant_scale;
    const int32_t scores_zp   = cfg.output_specs[1].quant_zero_point;
    const int64_t n_paxels  = boxes_t.shape[2];
    const int64_t n_classes = scores_t.shape[1];

    std::vector<Detection> accepted;
    for (int64_t idx = 0; idx < n_paxels; ++idx) {
        int64_t best = idx;
        for (int64_t num = idx + n_paxels; num < n_classes * n_paxels; num += n_paxels) {
            if (PostprocessUtils::readFloat(scores_t, num, scores_scale, scores_zp) >
                PostprocessUtils::readFloat(scores_t, best, scores_scale, scores_zp)) {
                best = num;
            }
        }
        // Reference's ParseDualblockFrame computes class_idx = idx / n_paxels
        // (always 0, an apparent copy-paste bug from the mono-block path) —
        // this port uses best / n_paxels, the actual scanned best channel,
        // matching mono-block's correct behavior (see header deviations note).
        const int class_idx = static_cast<int>(best / n_paxels);
        const float confidence = PostprocessUtils::readFloat(scores_t, best, scores_scale, scores_zp);
        if (confidence < conf_threshold) continue;

        const float cx = PostprocessUtils::readFloat(boxes_t, idx + 0 * n_paxels, boxes_scale, boxes_zp);
        const float cy = PostprocessUtils::readFloat(boxes_t, idx + 1 * n_paxels, boxes_scale, boxes_zp);
        const float w  = PostprocessUtils::readFloat(boxes_t, idx + 2 * n_paxels, boxes_scale, boxes_zp);
        const float h  = PostprocessUtils::readFloat(boxes_t, idx + 3 * n_paxels, boxes_scale, boxes_zp);

        Detection cand;
        cand.class_idx = class_idx;
        cand.name       = labelFor(class_idx, labels);
        cand.score      = confidence;
        cand.x1 = std::max(cx - w / 2.0f, 0.0f);
        cand.y1 = std::max(cy - h / 2.0f, 0.0f);
        cand.x2 = std::min(cand.x1 + w, static_cast<float>(input_w));
        cand.y2 = std::min(cand.y1 + h, static_cast<float>(input_h));

        acceptWithNMS(std::move(cand), accepted, iou_threshold);
    }
    return accepted;
}

// Triple-block (3 tensors) — tensor0 [1,n_paxels,4] boxes, already decoded
// to absolute (left,top,right,bottom) pixel coords per paxel; tensor1
// [1,n_paxels] scores; tensor2 [1,n_paxels] class indices (stored as float).
// This is the "post-argmax" export variant — no per-class score scan needed.
// Unlike mono-/dual-block (which clamp to the input frame), out-of-frame
// boxes are DISCARDED here.
std::vector<Detection> decodeTripleblock(
    const OutputTensor& boxes_t, const OutputTensor& scores_t, const OutputTensor& classes_t,
    const RequestConfig& cfg, const std::map<int, std::string>& labels,
    float conf_threshold, float iou_threshold,
    int64_t input_w, int64_t input_h) {

    const float boxes_scale   = cfg.output_specs[0].quant_scale;
    const int32_t boxes_zp     = cfg.output_specs[0].quant_zero_point;
    const float scores_scale  = cfg.output_specs[1].quant_scale;
    const int32_t scores_zp    = cfg.output_specs[1].quant_zero_point;
    const float classes_scale = cfg.output_specs[2].quant_scale;
    const int32_t classes_zp   = cfg.output_specs[2].quant_zero_point;
    const int64_t n_paxels = boxes_t.shape[1];

    std::vector<Detection> accepted;
    for (int64_t idx = 0; idx < n_paxels; ++idx) {
        const float confidence = PostprocessUtils::readFloat(scores_t, idx, scores_scale, scores_zp);
        if (confidence < conf_threshold) continue;

        const int class_idx = static_cast<int>(
            PostprocessUtils::readFloat(classes_t, idx, classes_scale, classes_zp));

        Detection cand;
        cand.class_idx = class_idx;
        cand.name       = labelFor(class_idx, labels);
        cand.score      = confidence;
        cand.x1 = PostprocessUtils::readFloat(boxes_t, idx * 4 + 0, boxes_scale, boxes_zp);
        cand.y1 = PostprocessUtils::readFloat(boxes_t, idx * 4 + 1, boxes_scale, boxes_zp);
        cand.x2 = PostprocessUtils::readFloat(boxes_t, idx * 4 + 2, boxes_scale, boxes_zp);
        cand.y2 = PostprocessUtils::readFloat(boxes_t, idx * 4 + 3, boxes_scale, boxes_zp);

        if (cand.x1 < 0.0f || cand.x1 > static_cast<float>(input_w) ||
            cand.x2 < 0.0f || cand.x2 > static_cast<float>(input_w) ||
            cand.y1 < 0.0f || cand.y1 > static_cast<float>(input_h) ||
            cand.y2 < 0.0f || cand.y2 > static_cast<float>(input_h)) {
            continue;
        }

        acceptWithNMS(std::move(cand), accepted, iou_threshold);
    }
    return accepted;
}

} // namespace

std::string Yolov8Postprocess::process(
    const std::vector<OutputTensor>& raw,
    const RequestConfig&             cfg) const {

    const auto& outputs = raw;

    // Model input spatial dims (NHWC: [1, H, W, C]) — used as the frame the
    // reference clamps/discards boxes against (region == whole input frame,
    // QAIServe has no region-of-interest concept). Falls back to YOLOv8's
    // common 640x640 export input if input_specs is unexpectedly empty.
    int64_t input_h = 640;
    int64_t input_w = 640;
    if (!cfg.input_specs.empty() && cfg.input_specs[0].shape.size() >= 3) {
        input_h = cfg.input_specs[0].shape[1];
        input_w = cfg.input_specs[0].shape[2];
    }

    // conf_threshold/iou_threshold are user-supplied and validated against
    // their valid ranges — confidence is always in [0,1] and IoU is a ratio
    // of areas so it's always in [0,1]. An out-of-range value falls back to
    // the default instead of silently rejecting every detection.
    const float conf_threshold = PostprocessUtils::getFloatInRange(
        cfg, "conf_threshold", kDefaultThreshold, 0.0f, 1.0f);
    const float iou_threshold = PostprocessUtils::getFloatInRange(
        cfg, "iou_threshold", kNMSIntersectionTreshold, 0.0f, 1.0f);

    // Class names come from labels.txt alongside the model's file
    // (one name per line, by position) — required: without names this
    // postprocess's whole output is meaningless. Throws (caught by
    // InferController via PostprocPlugin, surfaced as a 500) if the file
    // is missing/empty.
    const std::map<int, std::string> labels =
        PostprocessUtils::loadLabelsFile(cfg.labels_path, /*required=*/true);

    // Tensor arrangement varies by export tool/options — resolved from tensor count.
    std::vector<Detection> accepted;
    if (outputs.size() == 3) {
        accepted = decodeTripleblock(outputs[0], outputs[1], outputs[2], cfg, labels,
                                      conf_threshold, iou_threshold, input_w, input_h);
    } else if (outputs.size() == 2) {
        accepted = decodeDualblock(outputs[0], outputs[1], cfg, labels,
                                    conf_threshold, iou_threshold, input_w, input_h);
    } else if (outputs.size() == 1) {
        accepted = decodeMonoblock(outputs[0], cfg, labels,
                                    conf_threshold, iou_threshold, input_w, input_h);
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
        detections_json.push_back({
            {"name",  d.name},
            {"score", d.score},
            {"bbox",  {d.x1 * scale_x, d.y1 * scale_y, d.x2 * scale_x, d.y2 * scale_y}},
        });
    }

    nlohmann::json result;
    result["detections"]   = detections_json;
    result["image_width"]  = (cfg.image_width > 0) ? cfg.image_width : static_cast<int>(input_w);
    result["image_height"] = (cfg.image_height > 0) ? cfg.image_height : static_cast<int>(input_h);
    return result.dump();
}

postproc_abi::PluginDescription Yolov8Postprocess::pluginInfo() const {
    postproc_abi::PluginDescription d;
    d.name = "yolov8";
    d.description =
        "Returns a JSON object with: detections — array of accepted objects "
        "after confidence filtering and per-class NMS, each with: name — class "
        "label from labels.txt (\"unknown\" if unmapped); score — confidence "
        "in 0-1; bbox — [x1,y1,x2,y2] in the caller's image-space px. "
        "image_width, image_height — dims the bbox coords are scaled to "
        "(cfg.image_width/height if provided, otherwise model input dims). "
        "Input tensor arrangement varies by export tool/options, resolved at "
        "runtime from tensor count — see layouts below.";
    d.layouts = {
        {   // layout 0 — mono-block: [1, 4+n_classes, n_paxels]
            {{1, -1, -1}, {DataType::UINT8, DataType::FLOAT16, DataType::FLOAT32}},
        },
        {   // layout 1 — dual-block: [1,4,n_paxels] + [1,n_classes,n_paxels]
            {{1, 4, -1},  {DataType::UINT8, DataType::FLOAT16, DataType::FLOAT32}},
            {{1, -1, -1}, {DataType::UINT8, DataType::FLOAT16, DataType::FLOAT32}},
        },
        {   // layout 2 — triple-block: [1,n_paxels,4] + [1,n_paxels] + [1,n_paxels]
            {{1, -1, 4}, {DataType::UINT8, DataType::FLOAT16, DataType::FLOAT32}},
            {{1, -1},    {DataType::UINT8, DataType::FLOAT16, DataType::FLOAT32}},
            {{1, -1},    {DataType::UINT8, DataType::FLOAT16, DataType::FLOAT32}},
        },
    };
    d.parameters = {
        {"conf_threshold", "float",  "0.70", "Minimum detection confidence"},
        {"iou_threshold",  "float",  "0.5",  "Per-class NMS IoU overlap threshold"},
    };
    return d;
}

// ── bottom of postprocess-yolov8.cpp — ABI factory trio ────────────────────
POSTPROC_ABI_EXPORT(Yolov8Postprocess)
