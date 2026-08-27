// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "modules/postprocess-qpd.h"
#include "postprocess-utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>

using PostprocessUtils::Detection;
using PostprocessUtils::Keypoint;
using postproc_abi::DataType;
using postproc_abi::OutputTensor;
using postproc_abi::RequestConfig;

namespace {

// Landmarks: [{"id":0,"landmarks_names":[{"id":0,"name":"nose"}, ...]}, ...]
// class_idx -> (keypoint_id -> name). A class with no entry, or a keypoint
// id missing from its map, yields no name for that keypoint (see the
// findKeypointName lookup below) — the box is still emitted either way.
std::map<int, std::map<int, std::string>> parseLandmarkNames(const std::string& json_str) {
    std::map<int, std::map<int, std::string>> out;
    if (json_str.empty()) return out;
    try {
        nlohmann::json arr = nlohmann::json::parse(json_str);
        if (!arr.is_array()) return out;
        for (const auto& cls : arr) {
            if (!cls.is_object() || !cls.contains("id") || !cls.contains("landmarks_names")) continue;
            int class_id = cls.at("id").get<int>();
            std::map<int, std::string> names;
            for (const auto& lmk : cls.at("landmarks_names")) {
                if (!lmk.is_object() || !lmk.contains("id") || !lmk.contains("name")) continue;
                names[lmk.at("id").get<int>()] = lmk.at("name").get<std::string>();
            }
            out[class_id] = std::move(names);
        }
    } catch (const nlohmann::json::exception&) {
        // Malformed input fails closed — empty map, every keypoint unnamed/skipped.
    }
    return out;
}

const float kDefaultThreshold        = 0.70f;
const float kNMSIntersectionTreshold = 0.5f;
const float kBboxSizeTreshold        = 400.0f; // 20x20 px

} // namespace

std::string PersonDetPostprocess::process(
    const std::vector<OutputTensor>& raw,
    const RequestConfig&             cfg) const {

    // Fixed tensor order (see pluginInfo() below)
    const OutputTensor& scores    = raw[0];
    const OutputTensor& bboxes    = raw[1];
    const OutputTensor& landmarks = raw[2];
    const OutputTensor& lmkscores = raw[3];

    const float scores_scale    = cfg.output_specs[0].quant_scale;
    const int32_t scores_zp     = cfg.output_specs[0].quant_zero_point;
    const float bboxes_scale    = cfg.output_specs[1].quant_scale;
    const int32_t bboxes_zp     = cfg.output_specs[1].quant_zero_point;
    const float landmarks_scale = cfg.output_specs[2].quant_scale;
    const int32_t landmarks_zp  = cfg.output_specs[2].quant_zero_point;
    const float lmkscores_scale = cfg.output_specs[3].quant_scale;
    const int32_t lmkscores_zp  = cfg.output_specs[3].quant_zero_point;

    // Grid dims come from the scores tensor shape [1, grid_h, grid_w, n_classes].
    const int64_t grid_h    = scores.shape[1];
    const int64_t grid_w    = scores.shape[2];
    const int64_t n_classes = scores.shape[3];
    const int64_t n_landmarks = landmarks.shape[3] / 2;

    // Model input spatial dims (NHWC: [1, H, W, C]) — used to derive the
    // per-axis paxel size from the actual model, rather than hardcoding it.
    // Falls back to the reference model's known 480x640 input if input_specs
    // is unexpectedly empty (should not happen for a validated predictive model).
    int64_t input_h = 480;
    int64_t input_w = 640;
    if (!cfg.input_specs.empty() && cfg.input_specs[0].shape.size() >= 3) {
        input_h = cfg.input_specs[0].shape[1];
        input_w = cfg.input_specs[0].shape[2];
    }
    const float paxel_w = static_cast<float>(input_w) / static_cast<float>(grid_w);
    const float paxel_h = static_cast<float>(input_h) / static_cast<float>(grid_h);

    // conf_threshold/iou_threshold are user-supplied and validated against
    // their valid ranges — confidence is always in [0,1] and IoU is a ratio
    // of areas so it's always in [0,1]. An out-of-range value falls back to
    // the default instead of silently rejecting every detection. min_box_area
    // is not user-configurable — always the reference default.
    const float conf_threshold = PostprocessUtils::getFloatInRange(
        cfg, "conf_threshold", kDefaultThreshold, 0.0f, 1.0f);
    const float iou_threshold = PostprocessUtils::getFloatInRange(
        cfg, "iou_threshold", kNMSIntersectionTreshold, 0.0f, 1.0f);
    const float min_box_area = kBboxSizeTreshold;

    // Class names come from labels.txt alongside the model's config_file
    // (one name per line, by position) — required: without names this
    // postprocessor's whole output is meaningless. Throws (caught by
    // InferController via PostprocPlugin, surfaced as a 500) if the file
    // is missing/empty. Per-class keypoint names still
    // travel as a separate request query param (no plain-text file schema
    // for the nested class -> keypoint-id -> name mapping).
    const std::map<int, std::string> labels =
        PostprocessUtils::loadLabelsFile(cfg.labels_path, /*required=*/true);
    std::map<int, std::map<int, std::string>> landmark_names;
    {
        auto it = cfg.extra.find("landmarks");
        if (it != cfg.extra.end()) landmark_names = parseLandmarkNames(it->second);
    }

    std::vector<Detection> accepted;

    const int64_t n_paxels = grid_h * grid_w;
    for (int64_t idx = 0; idx < n_paxels * n_classes; ++idx) {
        const float confidence = PostprocessUtils::readFloat(scores, idx, scores_scale, scores_zp);
        if (confidence < conf_threshold) continue;

        const int class_idx = static_cast<int>(idx % n_classes);

        auto label_it = labels.find(class_idx);
        if (label_it == labels.end()) continue; // unknown label — skip, matches reference

        const int64_t cx = (idx / n_classes) % grid_w;
        const int64_t cy = (idx / n_classes) / grid_w;

        const size_t bbox_base = static_cast<size_t>(idx) * 4;
        const float bbox_l = PostprocessUtils::readFloat(bboxes, bbox_base + 0, bboxes_scale, bboxes_zp);
        const float bbox_t = PostprocessUtils::readFloat(bboxes, bbox_base + 1, bboxes_scale, bboxes_zp);
        const float bbox_r = PostprocessUtils::readFloat(bboxes, bbox_base + 2, bboxes_scale, bboxes_zp);
        const float bbox_b = PostprocessUtils::readFloat(bboxes, bbox_base + 3, bboxes_scale, bboxes_zp);

        Detection cand;
        cand.class_idx = class_idx;
        cand.name  = label_it->second;
        cand.score = confidence;
        cand.x1 = (static_cast<float>(cx) - bbox_l) * paxel_w;
        cand.y1 = (static_cast<float>(cy) - bbox_t) * paxel_h;
        cand.x2 = (static_cast<float>(cx) + bbox_r) * paxel_w;
        cand.y2 = (static_cast<float>(cy) + bbox_b) * paxel_h;

        const float box_area = (cand.x2 - cand.x1) * (cand.y2 - cand.y1);
        if (box_area < min_box_area) continue;

        cand.x1 = std::max(cand.x1, 0.0f);
        cand.y1 = std::max(cand.y1, 0.0f);
        cand.x2 = std::min(cand.x2, static_cast<float>(input_w));
        cand.y2 = std::min(cand.y2, static_cast<float>(input_h));

        // Incremental per-class NMS — only compares against already-accepted
        // boxes of the SAME class; an IoU above threshold either replaces a
        // lower-confidence accepted box or rejects the candidate.
        bool rejected = false;
        int  replace_idx = -1;
        for (size_t i = 0; i < accepted.size(); ++i) {
            if (accepted[i].class_idx != cand.class_idx) continue;
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

        // Keypoints for this grid cell — split channel layout (17 x-offsets
        // then 17 y-offsets), each with its own confidence in lmkscores.
        // Only kept if both above threshold and named for this class.
        auto names_it = landmark_names.find(class_idx);
        if (names_it != landmark_names.end()) {
            const std::map<int, std::string>& names = names_it->second;
            const int64_t paxel_idx = idx / n_classes;
            for (int64_t k = 0; k < n_landmarks; ++k) {
                const size_t score_idx = static_cast<size_t>(paxel_idx * n_landmarks + k);
                const float kp_score = PostprocessUtils::readFloat(lmkscores, score_idx, lmkscores_scale, lmkscores_zp);
                if (kp_score < conf_threshold) continue;

                auto name_it = names.find(static_cast<int>(k));
                if (name_it == names.end()) continue;

                const size_t lm_base = static_cast<size_t>(paxel_idx) * (n_landmarks * 2);
                const float ld_x = PostprocessUtils::readFloat(landmarks, lm_base + k, landmarks_scale, landmarks_zp);
                const float ld_y = PostprocessUtils::readFloat(landmarks, lm_base + k + n_landmarks, landmarks_scale, landmarks_zp);

                Keypoint kp;
                kp.name  = name_it->second;
                kp.x     = std::min(std::max((static_cast<float>(cx) + ld_x) * paxel_w, 0.0f), static_cast<float>(input_w));
                kp.y     = std::min(std::max((static_cast<float>(cy) + ld_y) * paxel_h, 0.0f), static_cast<float>(input_h));
                kp.score = kp_score;
                cand.landmarks.push_back(std::move(kp));
            }
        }

        accepted.push_back(std::move(cand));
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
        for (const auto& kp : d.landmarks) {
            landmarks_json.push_back({
                {"name",  kp.name},
                {"x",     kp.x * scale_x},
                {"y",     kp.y * scale_y},
                {"score", kp.score},
            });
        }
        detections_json.push_back({
            {"name",  d.name},
            {"score", d.score},
            {"bbox",  {d.x1 * scale_x, d.y1 * scale_y, d.x2 * scale_x, d.y2 * scale_y}},
            {"landmarks", landmarks_json},
        });
    }

    nlohmann::json result;
    result["detections"]   = detections_json;
    result["image_width"]  = (cfg.image_width > 0) ? cfg.image_width : static_cast<int>(input_w);
    result["image_height"] = (cfg.image_height > 0) ? cfg.image_height : static_cast<int>(input_h);
    return result.dump();
}

postproc_abi::PluginDescription PersonDetPostprocess::pluginInfo() const {
    postproc_abi::PluginDescription d;
    d.name = "person_detect_qpd";
    d.description =
        "Returns a JSON object with: detections — array of accepted objects "
        "after confidence filtering and per-class NMS, each with: name — class "
        "label from labels.txt; score — confidence in 0-1; bbox — "
        "[x1,y1,x2,y2] in the caller's image-space px; landmarks — array of "
        "named keypoints for this detection (empty if the class has no "
        "keypoint names configured), each with: name — keypoint name from the "
        "`landmarks` request query param; x, y — coords in the caller's "
        "image-space px; score — keypoint confidence in 0-1. image_width, "
        "image_height — dims the bbox/keypoint coords are scaled to "
        "(cfg.image_width/height if provided, otherwise model input dims). "
        "Class names come from labels.txt alongside the model's config_file — "
        "required; the request fails if the file is missing or empty. Keypoint "
        "names come from the `landmarks` request query param (JSON)";
    d.layouts = {
        {   // layout 0 — only supported tensor arrangement.
            {{1,120,160, 3},  {DataType::UINT8, DataType::FLOAT16, DataType::FLOAT32}},
            {{1,120,160,12},  {DataType::UINT8, DataType::FLOAT16, DataType::FLOAT32}},
            {{1,120,160,34},  {DataType::UINT8, DataType::FLOAT16, DataType::FLOAT32}},
            {{1,120,160,17},  {DataType::UINT8, DataType::FLOAT16, DataType::FLOAT32}},
        },
    };
    d.parameters = {
        {"conf_threshold", "float",  "0.70", "Minimum detection/keypoint confidence"},
        {"iou_threshold",  "float",  "0.5",  "Per-class NMS IoU overlap threshold"},
        {"landmarks",      "string", "",     "JSON array [{'id':0,'landmarks_names':[{'id':0,'name':'nose'}, ...]}, ...] — per-class keypoint names"},
    };
    return d;
}

// ── bottom of postprocess-qpd.cpp — ABI factory trio ───────────────────────
POSTPROC_ABI_EXPORT(PersonDetPostprocess)
