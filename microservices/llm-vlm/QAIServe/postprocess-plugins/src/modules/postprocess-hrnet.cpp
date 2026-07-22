// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "modules/postprocess-hrnet.h"
#include "postprocess-utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using PostprocessUtils::Keypoint;
using postproc_abi::DataType;
using postproc_abi::OutputTensor;
using postproc_abi::RequestConfig;

namespace {

std::string labelFor(int idx, const std::map<int, std::string>& labels) {
    auto it = labels.find(idx);
    return (it != labels.end()) ? it->second : "unknown";
}

const float kDefaultThreshold = 0.70f;

// Connections: [{"id":0,"connection":1}, ...] — same schema as the reference
// module's Configure() "connections" JSON. Each entry pairs two keypoint
// indices into one skeleton link; malformed/missing fields drop that entry.
std::vector<std::pair<int, int>> parseConnections(const std::string& json_str) {
    std::vector<std::pair<int, int>> out;
    if (json_str.empty()) return out;
    try {
        nlohmann::json arr = nlohmann::json::parse(json_str);
        if (!arr.is_array()) return out;
        for (const auto& item : arr) {
            if (!item.is_object() || !item.contains("id") || !item.contains("connection")) continue;
            out.emplace_back(item.at("id").get<int>(), item.at("connection").get<int>());
        }
    } catch (const nlohmann::json::exception&) {
        // Malformed input fails closed — no links, keypoints are still emitted.
    }
    return out;
}

} // namespace

std::string HrnetPostprocess::process(
    const std::vector<OutputTensor>& raw,
    const RequestConfig&             cfg) const {

    const OutputTensor& heatmap = raw[0];

    const float scale = cfg.output_specs[0].quant_scale;
    const int32_t zp   = cfg.output_specs[0].quant_zero_point;

    // Grid dims + channel count come from the heatmap tensor shape
    // [1, grid_h, grid_w, n_keypoints].
    const int64_t grid_h     = heatmap.shape[1];
    const int64_t grid_w     = heatmap.shape[2];
    const int64_t n_keypoints = heatmap.shape[3];
    const int64_t n_blocks   = grid_h * grid_w * n_keypoints;

    // Model input spatial dims (NHWC: [1, H, W, C]) — HRNet is a top-down,
    // single-pose estimator: the model input is assumed to already be a crop
    // containing exactly one person, so keypoints are scaled directly into this frame rather
    // than a separate capture region. Falls back to the reference model's
    // known 256x256 input if input_specs is unexpectedly empty.
    int64_t input_h = 256;
    int64_t input_w = 256;
    if (!cfg.input_specs.empty() && cfg.input_specs[0].shape.size() >= 3) {
        input_h = cfg.input_specs[0].shape[1];
        input_w = cfg.input_specs[0].shape[2];
    }

    // conf_threshold is user-supplied and validated against its valid range —
    // it's compared against the mean per-keypoint confidence, which is
    // always in [0,1] (raw heatmap value, no sigmoid — see header note). An
    // out-of-range value falls back to the default instead of silently
    // rejecting (or accepting) every pose.
    const float conf_threshold = PostprocessUtils::getFloatInRange(
        cfg, "conf_threshold", kDefaultThreshold, 0.0f, 1.0f);

    // Keypoint names come from labels.txt alongside the model's file
    // (see PostprocConfig::labels_path) — optional: pose coordinates remain
    // useful even unnamed, so a missing/unreadable/empty file just yields
    // "unknown" names via labelFor() (required=false, unlike the
    // detection/classification/segmentation postprocessors). Skeleton links
    // still travel as the `connections` request query param.
    std::map<int, std::string> labels =
        PostprocessUtils::loadLabelsFile(cfg.labels_path, /*required=*/false);
    std::vector<std::pair<int, int>> connections;
    {
        auto it = cfg.extra.find("connections");
        if (it != cfg.extra.end()) connections = parseConnections(it->second);
    }

    // flatIdx — standard NHWC flat index.
    auto flatIdx = [n_keypoints, grid_w](int64_t row, int64_t col, int64_t channel) {
        return static_cast<size_t>(((row * grid_w) + col) * n_keypoints + channel);
    };

    std::vector<Keypoint> keypoints(n_keypoints);
    float total_confidence = 0.0f;

    for (int64_t idx = 0; idx < n_keypoints; ++idx) {
        // Argmax over every spatial position for this keypoint channel
        int64_t best = idx;
        for (int64_t num = idx + n_keypoints; num < n_blocks; num += n_keypoints) {
            if (PostprocessUtils::readFloat(heatmap, num, scale, zp) >
                PostprocessUtils::readFloat(heatmap, best, scale, zp)) {
                best = num;
            }
        }

        const float confidence = PostprocessUtils::readFloat(heatmap, best, scale, zp);
        const int64_t x = (best / n_keypoints) % grid_w;
        const int64_t y = (best / n_keypoints) / grid_w;

        // Sub-pixel refinement: nudge a quarter-cell toward whichever
        // horizontal/vertical neighbor scores higher.
        int dx = 0, dy = 0;
        if (x > 1 && x < grid_w - 1 && y > 0 && y < grid_h) {
            const float left  = PostprocessUtils::readFloat(heatmap, flatIdx(y, x + 1, idx), scale, zp);
            const float right = PostprocessUtils::readFloat(heatmap, flatIdx(y, x - 1, idx), scale, zp);
            dx = (left > right) ? 1 : (left < right) ? -1 : 0;
        }
        if (y > 1 && y < grid_h - 1 && x > 0 && x < grid_w) {
            const float below = PostprocessUtils::readFloat(heatmap, flatIdx(y + 1, x, idx), scale, zp);
            const float above = PostprocessUtils::readFloat(heatmap, flatIdx(y - 1, x, idx), scale, zp);
            dy = (below > above) ? 1 : (below < above) ? -1 : 0;
        }

        Keypoint& kp = keypoints[idx];
        kp.x = ((static_cast<float>(x) + dx * 0.25f) / static_cast<float>(grid_w)) * static_cast<float>(input_w);
        kp.y = ((static_cast<float>(y) + dy * 0.25f) / static_cast<float>(grid_h)) * static_cast<float>(input_h);
        kp.x = std::min(std::max(kp.x, 0.0f), static_cast<float>(input_w));
        kp.y = std::min(std::max(kp.y, 0.0f), static_cast<float>(input_h));
        kp.score = confidence;
        kp.name  = labelFor(static_cast<int>(idx), labels);

        total_confidence += confidence;
    }

    const float pose_confidence = total_confidence / static_cast<float>(n_keypoints);

    // Scale from model-space pixels to the caller's original image dims if
    // provided; otherwise leave coordinates in model-space (input_w x input_h).
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    if (cfg.image_width > 0 && cfg.image_height > 0) {
        scale_x = static_cast<float>(cfg.image_width) / static_cast<float>(input_w);
        scale_y = static_cast<float>(cfg.image_height) / static_cast<float>(input_h);
    }

    nlohmann::json result;
    result["poses"] = nlohmann::json::array();

    // Below-threshold poses are dropped.
    if (pose_confidence >= conf_threshold) {
        nlohmann::json keypoints_json = nlohmann::json::array();
        for (const auto& kp : keypoints) {
            keypoints_json.push_back({
                {"name",       kp.name},
                {"x",          kp.x * scale_x},
                {"y",          kp.y * scale_y},
                {"confidence", kp.score},
            });
        }

        nlohmann::json links_json = nlohmann::json::array();
        for (const auto& link : connections) {
            if (link.first < 0 || link.first >= static_cast<int>(n_keypoints) ||
                link.second < 0 || link.second >= static_cast<int>(n_keypoints)) {
                continue;
            }
            const Keypoint& from = keypoints[link.first];
            const Keypoint& to   = keypoints[link.second];
            links_json.push_back({
                {"from", {{"name", from.name}, {"x", from.x * scale_x}, {"y", from.y * scale_y}}},
                {"to",   {{"name", to.name},   {"x", to.x * scale_x},   {"y", to.y * scale_y}}},
            });
        }

        result["poses"].push_back({
            {"confidence", pose_confidence},
            {"keypoints",  keypoints_json},
            {"links",      links_json},
        });
    }

    result["image_width"]  = (cfg.image_width > 0) ? cfg.image_width : static_cast<int>(input_w);
    result["image_height"] = (cfg.image_height > 0) ? cfg.image_height : static_cast<int>(input_h);
    return result.dump();
}

postproc_abi::PluginDescription HrnetPostprocess::pluginInfo() const {
    postproc_abi::PluginDescription d;
    d.name = "hrnet";
    d.description =
        "Returns a JSON object with: poses — array with at most one entry "
        "(top-down single-pose model, no per-cell multi-instance detection), "
        "omitted entirely if mean keypoint confidence misses conf_threshold; "
        "each entry has: confidence — mean per-keypoint confidence in 0-1; "
        "keypoints — array of all keypoints (unnamed ones report \"unknown\"), "
        "each with: name — keypoint name from labels.txt; x, y — coords in the "
        "caller's image-space px; confidence — per-keypoint confidence in 0-1; "
        "links — skeleton links requested via the `connections` query param, "
        "each with: from, to — {name, x, y} of the two connected keypoints. "
        "image_width, image_height — dims the coords are scaled to "
        "(cfg.image_width/height if provided, otherwise model input dims). "
        "Keypoint names come from labels.txt alongside the model's config_file — "
        "optional; a missing/unreadable/empty file yields \"unknown\" names "
        "rather than failing the request, since pose coordinates remain "
        "useful even unnamed. "
        "Skeleton links come from the `connections` request query param, "
        "not hardcoded.";
    d.layouts = {
        {   // layout 0 — only supported tensor arrangement.
            {{1, -1, -1, -1}, {DataType::UINT8, DataType::FLOAT16, DataType::FLOAT32}},
        },
    };
    d.parameters = {
        {"conf_threshold", "float",  "0.70", "Minimum mean per-keypoint confidence for the pose to be reported"},
        {"connections",    "string", "",     "JSON array [{'id':0,'connection':1}, ...] — pairs of keypoint indices to report as skeleton links"},
    };
    return d;
}

// ── bottom of postprocess-hrnet.cpp — ABI factory trio ─────────────────────
POSTPROC_ABI_EXPORT(HrnetPostprocess)
