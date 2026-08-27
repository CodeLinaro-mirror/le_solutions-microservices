// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "postproc/PostprocPlugin.h"
#include "postproc/PostprocAbiConvert.h"

PostprocPlugin::PostprocPlugin(postproc_abi::IPostprocess* instance, DestroyFn destroy)
    : instance_(instance), destroy_(destroy) {}

PostprocPlugin::~PostprocPlugin() {
    if (instance_ && destroy_) destroy_(instance_);
}

nlohmann::json PostprocPlugin::process(
    const TensorInferenceResponse& raw,
    const PostprocConfig&          cfg) {

    std::vector<postproc_abi::OutputTensor> abi_outputs;
    abi_outputs.reserve(raw.outputs.size());
    for (const auto& t : raw.outputs) abi_outputs.push_back(postproc_convert::toAbi(t));

    const postproc_abi::RequestConfig abi_cfg = postproc_convert::toAbi(cfg);

    // Plugin serializes its own result as a JSON string (nlohmann::json::dump()
    // internally) — parse() here re-materializes it as the json object every
    // other IPostprocess::process() returns. A malformed string throws,
    // propagating up through InferController's existing try/catch as a 500,
    // same as any other postprocess error.
    const std::string result_json = instance_->process(abi_outputs, abi_cfg);
    return nlohmann::json::parse(result_json);
}
