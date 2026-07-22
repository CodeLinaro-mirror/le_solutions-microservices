// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "modules/postprocess-deeplab-argmax.h"
#include "postprocess-utils.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>

using postproc_abi::DataType;
using postproc_abi::OutputTensor;
using postproc_abi::RequestConfig;

namespace {

// Colors: [{"id":0,"color":"5548f8ff"}, ...] — per-class legend color, hex
// string reported verbatim (not decoded). Class names now come from
// labels.txt instead of this JSON, so only "id"/"color" are read here.
// Fails closed on malformed/empty input.
std::map<int, std::string> parseColors(const std::string& json_str) {
    std::map<int, std::string> out;
    if (json_str.empty()) return out;
    try {
        nlohmann::json arr = nlohmann::json::parse(json_str);
        if (!arr.is_array()) return out;
        for (const auto& item : arr) {
            if (!item.is_object() || !item.contains("id") || !item.contains("color")) continue;
            out[item.at("id").get<int>()] = item.at("color").get<std::string>();
        }
    } catch (const nlohmann::json::exception&) {
        // Malformed input fails closed — empty map, every class id fully transparent.
    }
    return out;
}

// A class id with no matching label entry is named "unknown" and reported fully transparent.
std::pair<std::string, std::string> labelAndColorFor(
    int class_id,
    const std::map<int, std::string>& labels,
    const std::map<int, std::string>& colors) {

    auto label_it = labels.find(class_id);
    if (label_it == labels.end()) return {"unknown", "00000000"};

    auto color_it = colors.find(class_id);
    const std::string color = (color_it != colors.end()) ? color_it->second : "";
    return {label_it->second, color};
}

} // namespace

std::string DeeplabArgmaxPostprocess::process(
    const std::vector<OutputTensor>& raw,
    const RequestConfig&             cfg) const {

    const OutputTensor& tensor = raw[0];

    const float scale = cfg.output_specs[0].quant_scale;
    const int32_t zp   = cfg.output_specs[0].quant_zero_point;

    // Grid dims come from the tensor shape. Rank 4 [1,grid_h,grid_w,n_classes]
    // carries per-pixel class scores (argmax needed); rank 3 [1,grid_h,grid_w]
    // already holds the class id per pixel
    const bool has_class_dim = (tensor.shape.size() == 4);
    const int64_t grid_h    = tensor.shape[1];
    const int64_t grid_w    = tensor.shape[2];
    const int64_t n_classes = has_class_dim ? tensor.shape[3] : 1;

    // Class names come from labels.txt alongside the model's file
    // (one name per line, by position) — required: without names this
    // postprocessor's whole output is meaningless. Throws (caught by
    // InferController via PostprocPlugin, surfaced as a 500) if the file
    // is missing/empty. Colors still travel as a separate
    // request query param (no plain-text file schema for per-class color).
    const std::map<int, std::string> labels =
        PostprocessUtils::loadLabelsFile(cfg.labels_path, /*required=*/true);
    std::map<int, std::string> colors;
    {
        auto it = cfg.extra.find("colors");
        if (it != cfg.extra.end()) colors = parseColors(it->second);
    }

    // Output resolution: the caller's requested image dimensions if
    // supplied, otherwise the tensor's native grid — consistent with every
    // other postprocess's image_width/image_height convention.
    const int64_t out_w = (cfg.image_width  > 0) ? cfg.image_width  : grid_w;
    const int64_t out_h = (cfg.image_height > 0) ? cfg.image_height : grid_h;

    std::vector<int32_t> mask;
    mask.reserve(static_cast<size_t>(out_w * out_h));
    std::set<int32_t> seen_classes;

    for (int64_t row = 0; row < out_h; ++row) {
        // Nearest-neighbor row in the tensor grid.
        const int64_t gy = (row * grid_h) / out_h;

        for (int64_t col = 0; col < out_w; ++col) {
            // Nearest-neighbor column in the tensor grid.
            const int64_t gx = (col * grid_w) / out_w;
            const int64_t base = (gy * grid_w + gx) * n_classes;

            int32_t class_id;
            if (!has_class_dim) {
                // No 4th dimension: the tensor pixel already contains the
                // class id, stored as a float (Module::Process: `id = indata[id]`).
                class_id = static_cast<int32_t>(
                    PostprocessUtils::readFloat(tensor, base, scale, zp));
            } else {
                // Argmax over the class-score channels at this pixel
                int64_t best = base;
                for (int64_t num = base + 1; num < base + n_classes; ++num) {
                    if (PostprocessUtils::readFloat(tensor, num, scale, zp) >
                        PostprocessUtils::readFloat(tensor, best, scale, zp)) {
                        best = num;
                    }
                }
                class_id = static_cast<int32_t>(best - base);
            }

            mask.push_back(class_id);
            seen_classes.insert(class_id);
        }
    }

    nlohmann::json legend_json = nlohmann::json::array();
    for (int32_t class_id : seen_classes) {
        auto [name, color] = labelAndColorFor(class_id, labels, colors);
        legend_json.push_back({
            {"class_id", class_id},
            {"name",     name},
            {"color",    color},
        });
    }

    nlohmann::json result;
    result["mask"]         = mask;
    result["legend"]       = legend_json;
    result["image_width"]  = static_cast<int>(out_w);
    result["image_height"] = static_cast<int>(out_h);
    return result.dump();
}

postproc_abi::PluginDescription DeeplabArgmaxPostprocess::pluginInfo() const {
    postproc_abi::PluginDescription d;
    d.name = "deeplab_argmax";
    d.description =
        "Returns a JSON object with: mask — flat array of image_width * "
        "image_height class ids, row-major, one id per pixel (nearest-neighbor "
        "resampled from the tensor's native grid if image_width/image_height "
        "were supplied); legend — array of the classes that actually appear in "
        "mask, each with: class_id — id as it appears in mask; name — class "
        "label from labels.txt (\"unknown\" if unmapped); color — hex color "
        "string from the `colors` request query param (\"\" if unmapped, or the "
        "class is \"unknown\" in which case it's fully-transparent "
        "\"00000000\"). image_width, image_height — dims of mask "
        "(cfg.image_width/height if provided, otherwise the tensor's native "
        "grid dims).";
    d.layouts = {
        {   // layout 0 — rank 3, class id already resolved per pixel.
            {{1, -1, -1}, {DataType::UINT8, DataType::FLOAT16, DataType::FLOAT32}},
        },
        {   // layout 1 — rank 4, per-pixel class scores
            {{1, -1, -1, -1}, {DataType::UINT8, DataType::FLOAT16, DataType::FLOAT32}},
        },
    };
    d.parameters = {
        {"colors",      "string", "", "JSON array [{'id':0,'color':'5548f8ff'}, ...] — class index to legend color; unmapped indices report fully transparent"},
    };
    return d;
}

// ── bottom of postprocess-deeplab-argmax.cpp — ABI factory trio ────────────
POSTPROC_ABI_EXPORT(DeeplabArgmaxPostprocess)
