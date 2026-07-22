// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "modules/postprocess-ssd-mobilenet.h"
#include "postprocess-utils.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <map>
#include <memory>
#include <string>

using PostprocessUtils::Detection;
using postproc_abi::DataType;
using postproc_abi::OutputTensor;
using postproc_abi::RequestConfig;

namespace {

std::string labelFor(int class_idx, const std::map<int, std::string>& labels) {
    auto it = labels.find(class_idx);
    return (it != labels.end()) ? it->second : "unknown";
}

const float kDefaultThreshold = 0.70f;

} // namespace

std::string MobilenetSsdPostprocess::process(
    const std::vector<OutputTensor>& raw,
    const RequestConfig&             cfg) const {

    const OutputTensor& tensor = raw[0];

    const float scale  = cfg.output_specs[0].quant_scale;
    const int32_t zp    = cfg.output_specs[0].quant_zero_point;
    const int64_t n_classes = tensor.shape[1];

    // conf_threshold is user-supplied and validated against its valid range —
    // this compares the raw 0-1 probability directly (see header
    // deviations note), so confidence is always in [0,1]. An out-of-range
    // value falls back to the default instead of silently rejecting (or
    // accepting) every detection.
    const float conf_threshold = PostprocessUtils::getFloatInRange(
        cfg, "conf_threshold", kDefaultThreshold, 0.0f, 1.0f);

    // Class names come from labels.txt alongside the model's file
    // (one name per line, by position) — required: without names this
    // postprocess's whole output is meaningless. Throws (caught by
    // InferController via PostprocPlugin, surfaced as a 500) if the file
    // is missing/empty.
    const std::map<int, std::string> labels =
        PostprocessUtils::loadLabelsFile(cfg.labels_path, /*required=*/true);

    // Softmax over the raw logits: sum of exponents first, then each class's
    // share of that sum.
    double sum = 0.0;
    for (int64_t idx = 0; idx < n_classes; ++idx) {
        sum += std::exp(static_cast<double>(
            PostprocessUtils::readFloat(tensor, idx, scale, zp)));
    }

    std::vector<Detection> accepted;
    for (int64_t idx = 0; idx < n_classes; ++idx) {
        const float logit = PostprocessUtils::readFloat(tensor, idx, scale, zp);
        const float confidence = static_cast<float>(std::exp(static_cast<double>(logit)) / sum);

        // Discard results with confidence below the set threshold.
        if (confidence < conf_threshold) continue;

        Detection entry;
        entry.class_idx = static_cast<int>(idx);
        entry.name  = labelFor(entry.class_idx, labels);
        entry.score = confidence;
        accepted.push_back(std::move(entry));
    }

    nlohmann::json classifications_json = nlohmann::json::array();
    for (const auto& c : accepted) {
        classifications_json.push_back({
            {"name",       c.name},
            {"confidence", c.score},
        });
    }

    nlohmann::json result;
    result["classifications"] = classifications_json;
    return result.dump();
}

postproc_abi::PluginDescription MobilenetSsdPostprocess::pluginInfo() const {
    postproc_abi::PluginDescription d;
    d.name = "mobilenet_softmax";
    d.description =
        "Returns a JSON object with: classifications — array of accepted classes "
        "after confidence filtering (softmax applied by this postprocess, not the "
        "model), each with: name — class label from labels.txt; confidence — "
        "post-softmax probability in 0-1. Input tensor: [1, n_classes] "
        "UINT8|FP16|FP32 — raw (pre-softmax) per-class logits, n_classes an open "
        "wildcard.";
    d.layouts = {
        {   // layout 0 — only supported tensor arrangement.
            {{1, -1}, {DataType::UINT8, DataType::FLOAT16, DataType::FLOAT32}},
        },
    };
    d.parameters = {
        {"conf_threshold", "float",  "0.70", "Minimum post-softmax class confidence"},
    };
    return d;
}

// ── bottom of postprocess-ssd-mobilenet.cpp — ABI factory trio ─────────────
POSTPROC_ABI_EXPORT(MobilenetSsdPostprocess)
