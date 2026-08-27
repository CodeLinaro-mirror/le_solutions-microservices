// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include "postproc/PostprocPlugin.h"
#include "qai_forge/dto/TensorDTOs.h"
#include "qai_forge/managers/ModelConfigManager.h"

#include <memory>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// TensorExpectation — what one output tensor must look like for a
// postprocess plugin to be compatible with a model. Shape match and a set of 
// acceptable dtypes. One TensorExpectation per output tensor
// in one PostprocessInfo::layouts[] entry. Each shape dimension is either
// an exact size or -1 (open wildcard, matches any size).
// ─────────────────────────────────────────────────────────────────────────────
struct TensorExpectation {
    std::vector<int64_t>        shape;           // per-dim: exact or -1 wildcard
    std::vector<TensorDataType> accepted_dtypes;  // {UINT8, FP16, FP32} covers all precision variants
};

// ─────────────────────────────────────────────────────────────────────────────
// ParameterSchema — a single query parameter a postprocess accepts.
// Serialised into discovery endpoints so clients know what to pass without
// hardcoding anything.
// ─────────────────────────────────────────────────────────────────────────────
struct ParameterSchema {
    std::string name;         // e.g. "conf_threshold"
    std::string type;         // "float" | "int" | "bool"
    std::string default_val;  // e.g. "0.5"
    std::string description;
};

// ─────────────────────────────────────────────────────────────────────────────
// PostprocessInfo — one entry in the registry per postprocess.
//
// `layouts` is a list of tensor layout variants — each layout is a positional
// list of TensorExpectation. The compatibility check tries each layout in
// order and returns the first match along with its index. This allows one
// postprocess to handle architecturally equivalent models that differ only
// in tensor arrangement (e.g. grid vs flat output).
//
// unique_ptr is move-only, so PostprocessInfo is also move-only. The
// registry stores entries in a std::vector and inserts via
// emplace_back(std::move(info)).
// ─────────────────────────────────────────────────────────────────────────────
struct PostprocessInfo {
    std::string                                  name;        // registry key
    std::string                                  description; // info
    std::vector<std::vector<TensorExpectation>>  layouts;     // each entry is one supported tensor layout
    std::vector<ParameterSchema>                 parameters;  // surfaced in discovery endpoints
    std::unique_ptr<PostprocPlugin>              instance;    // created once at startup, lives forever
};

// ─────────────────────────────────────────────────────────────────────────────
// PostprocRegistry — Meyer's singleton, single source of truth for every
// postprocess the server knows about.
// ─────────────────────────────────────────────────────────────────────────────
class PostprocRegistry {
public:
    static PostprocRegistry& getInstance();  // Meyer's singleton

    // Scans `dir` for *.so files (non-recursive) and attempts to load each as
    // a postprocess plugin. A missing or empty directory is logged and
    // skipped, not fatal. Each successfully-loaded plugin is registered under
    // its own name() — if that name is already registered (by an earlier
    // directory scan or an earlier .so in this same scan), the new one is
    // logged as skipped and the earlier registration wins, so a
    // client-supplied plugin can never shadow a built-in.
    //
    // Call once per directory. Never hot-reloaded — adding/updating
    // a plugin requires a process restart. A successfully-loaded plugin's
    // .so is never dlclose()'d: the instance and the code backing its vtable
    // live for the process lifetime, matching PostprocessInfo::instance's
    // "created once, lives forever" contract.
    void loadDirectory(const std::string& dir);

    // Called once per postprocess at startup via static initializer in each .cpp
    void registerPostprocess(PostprocessInfo info);

    // Used by InferController: look up by name from ?postprocess= query param
    // Returns nullptr if not found
    const PostprocessInfo* find(const std::string& name) const;

    // Used by GET /v2/postprocesses: full catalog
    std::vector<const PostprocessInfo*> list() const;

    // Used by GET /v2/models/{model}: filter by compatibility with this model's outputs
    std::vector<const PostprocessInfo*>
        getCompatible(const std::vector<ModelTensorSpec>& output_specs) const;

    // Compatibility-check implementation. Tries each entry in `layouts`
    // in order and returns the index of the first one that matches `output_specs`
    // positionally (count, shape — exact or -1 wildcard per dim — and dtype
    // membership), or -1 if none match.
    static int matchLayout(const std::vector<std::vector<TensorExpectation>>& layouts,
                            const std::vector<ModelTensorSpec>& output_specs);

private:
    // Loads one .so, validates its ABI trio + version, and registers it into
    // this registry. Never dlclose()s on success — the loaded instance (and
    // the code backing its vtable) is assumed to live for the process
    // lifetime.
    void loadOne(const std::string& path);

    std::vector<PostprocessInfo> entries_;
};
