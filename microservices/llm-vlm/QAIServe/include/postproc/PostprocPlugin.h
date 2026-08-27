// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "postproc/PostprocConfig.h"
#include "postproc_abi/PostprocAbi.h"
#include "qai_forge/dto/TensorDTOs.h"

#include <nlohmann/json.hpp>

#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// PostprocPlugin — a single postprocess loaded from a plugin .so.
//
// Owns one loaded postproc_abi::IPostprocess instance plus its destroy
// function, and bridges process() calls across the ABI boundary by
// converting to/from the postproc_abi types (see PostprocAbiConvert.h).
// Constructed only by PostprocRegistry, which owns the dlopen/scanning
// mechanics (see PostprocRegistry::loadDirectory()/loadOne()) and holds
// each instance for the process lifetime.
//
// Thread-safety: process() is invoked concurrently across the detached
// std::thread QAIServe spawns per /infer_postprocess request, so it must
// stay stateless — no mutable member state beyond the plugin handle set up
// once at load time.
// ─────────────────────────────────────────────────────────────────────────────
class PostprocPlugin {
public:
    PostprocPlugin(const PostprocPlugin&) = delete;
    PostprocPlugin& operator=(const PostprocPlugin&) = delete;

    ~PostprocPlugin();

    nlohmann::json process(
        const TensorInferenceResponse& raw,
        const PostprocConfig&          cfg);

private:
    friend class PostprocRegistry;

    using DestroyFn = void (*)(postproc_abi::IPostprocess*);

    PostprocPlugin(postproc_abi::IPostprocess* instance, DestroyFn destroy);

    postproc_abi::IPostprocess* instance_;
    DestroyFn                     destroy_;
};
