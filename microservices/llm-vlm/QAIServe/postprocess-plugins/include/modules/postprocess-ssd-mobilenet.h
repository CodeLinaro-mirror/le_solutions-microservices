// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "postproc_abi/PostprocAbi.h"

// ─────────────────────────────────────────────────────────────────────────────
// MobilenetSsdPostprocess — decodes a single-tensor image-classification
// model's raw (pre-softmax) logits into per-class probabilities.
//
// One output tensor [1, n_classes]
// UINT8|FP16|FP32; n_classes is an open wildcard — softmax is
// applied here (the model itself does not), by summing exp() over every
// class then dividing each exp() by that sum:
//   confidence[idx] = exp(logits[idx]) / sum(exp(logits[*]))
//
// Class names come from labels.txt alongside the model's file (one
// name per line, by position) — required; the request fails if the file
// is missing or empty (see PostprocessUtils::loadLabelsFile).A class index
// with no matching label entry is reported as "unknown" rather than dropped.
// ─────────────────────────────────────────────────────────────────────────────
class MobilenetSsdPostprocess : public postproc_abi::IPostprocess {
public:
    postproc_abi::PluginDescription pluginInfo() const override;

    std::string process(
        const std::vector<postproc_abi::OutputTensor>& raw,
        const postproc_abi::RequestConfig&              cfg) const override;
};
