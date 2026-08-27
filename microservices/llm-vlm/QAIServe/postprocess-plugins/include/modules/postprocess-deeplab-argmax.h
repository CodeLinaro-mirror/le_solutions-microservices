// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "postproc_abi/PostprocAbi.h"

// ─────────────────────────────────────────────────────────────────────────────
// DeeplabArgmaxPostprocess — decodes a semantic-segmentation model's
// per-pixel class output into a flat class-id mask plus a legend of the
// classes that actually appear in it.
//
// One output tensor, in either of two ranks :
//   - rank 3: [1, grid_h, grid_w]              — each pixel already holds
//             its class id (as a float), no per-pixel argmax needed.
//   - rank 4: [1, grid_h, grid_w, n_classes]    — each pixel holds n_classes
//             raw scores; the class id is the argmax channel
// grid_h/grid_w/n_classes are open wildcards here.
//
// Class names come from labels.txt alongside the model's file (one
// name per line, by position) — required; the request fails if the file
// is missing or empty (see PostprocessUtils::loadLabelsFile). Colors, unlike
// names, still travel as the `colors` request query param (JSON:
// [{"id":0,"color":"5548f8ff"}, ...])
// — no plain-text file schema for per-class color, and this port DOES parse
// and report color; QAIServe has no rendering step, so the color is handed to
// the caller instead, in the legend rather than per-pixel (to keep the
// payload from ballooning to one color string per pixel).
// ─────────────────────────────────────────────────────────────────────────────
class DeeplabArgmaxPostprocess : public postproc_abi::IPostprocess {
public:
    postproc_abi::PluginDescription pluginInfo() const override;

    std::string process(
        const std::vector<postproc_abi::OutputTensor>& raw,
        const postproc_abi::RequestConfig&              cfg) const override;
};
