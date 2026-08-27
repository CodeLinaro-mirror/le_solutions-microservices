// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "postproc/PostprocRegistry.h"
#include "postproc/PostprocAbiConvert.h"

#include <dirent.h>
#include <dlfcn.h>

#include <algorithm>
#include <iostream>

namespace {

using CreateFn  = postproc_abi::IPostprocess* (*)();
using DestroyFn = void (*)(postproc_abi::IPostprocess*);
using VersionFn = const char* (*)();

// Lists regular files directly under `dir` whose name ends in ".so"
// (non-recursive — plugins are expected to be flat single-file .so drops).
// Returns an empty list (logged, not fatal) if `dir` doesn't exist or can't
// be opened.
std::vector<std::string> listSharedObjects(const std::string& dir) {
    std::vector<std::string> out;

    DIR* d = opendir(dir.c_str());
    if (!d) {
        std::cout << "[PostprocRegistry] directory not found or unreadable, skipping: "
                  << dir << std::endl;
        return out;
    }

    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        const std::string name = entry->d_name;
        if (name.size() > 3 && name.compare(name.size() - 3, 3, ".so") == 0) {
            out.push_back(dir + "/" + name);
        }
    }
    closedir(d);
    return out;
}

}  // namespace

PostprocRegistry& PostprocRegistry::getInstance() {
    static PostprocRegistry instance;
    return instance;
}

void PostprocRegistry::registerPostprocess(PostprocessInfo info) {
    entries_.emplace_back(std::move(info));
}

const PostprocessInfo* PostprocRegistry::find(const std::string& name) const {
    for (const auto& entry : entries_) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

std::vector<const PostprocessInfo*> PostprocRegistry::list() const {
    std::vector<const PostprocessInfo*> out;
    out.reserve(entries_.size());
    for (const auto& entry : entries_) out.push_back(&entry);
    return out;
}

std::vector<const PostprocessInfo*>
PostprocRegistry::getCompatible(const std::vector<ModelTensorSpec>& output_specs) const {
    std::vector<const PostprocessInfo*> out;
    for (const auto& entry : entries_) {
        if (matchLayout(entry.layouts, output_specs) >= 0) {
            out.push_back(&entry);
        }
    }
    return out;
}

// Positional match: tensor count, shape and dtype
// membership in accepted_dtypes. Tensor names are ignored. Tries each layout
// in order and returns the index of the first match, or -1 if none match.
int PostprocRegistry::matchLayout(
    const std::vector<std::vector<TensorExpectation>>& layouts,
    const std::vector<ModelTensorSpec>& output_specs) {

    for (size_t layout_idx = 0; layout_idx < layouts.size(); ++layout_idx) {
        const auto& layout = layouts[layout_idx];
        if (layout.size() != output_specs.size()) continue;

        bool matches = true;
        for (size_t i = 0; i < layout.size() && matches; ++i) {
            const TensorExpectation& expect = layout[i];
            const ModelTensorSpec&   actual = output_specs[i];

            if (expect.shape.size() != actual.shape.size()) { matches = false; break; }
            for (size_t d = 0; d < expect.shape.size(); ++d) {
                if (expect.shape[d] != -1 && expect.shape[d] != actual.shape[d]) { matches = false; break; }
            }
            if (!matches) break;

            TensorDataType actual_dtype = tensorDataTypeFromString(actual.dtype);
            bool dtype_ok = std::find(expect.accepted_dtypes.begin(),
                                       expect.accepted_dtypes.end(),
                                       actual_dtype) != expect.accepted_dtypes.end();
            if (!dtype_ok) { matches = false; break; }
        }

        if (matches) return static_cast<int>(layout_idx);
    }
    return -1;
}

// Loads one .so, validates its ABI trio + version, and registers it into
// this registry. Never dlclose()s on success — the loaded instance (and the
// code backing its vtable) is assumed to live for the process lifetime.
void PostprocRegistry::loadOne(const std::string& path) {
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        std::cerr << "[PostprocRegistry] dlopen failed for " << path
                   << ": " << dlerror() << std::endl;
        return;
    }

    dlerror();  // clear any pending error before dlsym, per dlsym(3) convention
    auto* create_fn  = reinterpret_cast<CreateFn>(dlsym(handle, "CreatePostprocess"));
    auto* destroy_fn = reinterpret_cast<DestroyFn>(dlsym(handle, "DestroyPostprocess"));
    auto* version_fn = reinterpret_cast<VersionFn>(dlsym(handle, "PostprocAbiVersion"));

    if (!create_fn || !destroy_fn || !version_fn) {
        std::cerr << "[PostprocRegistry] " << path
                   << " is missing required ABI symbol(s) (CreatePostprocess/"
                      "DestroyPostprocess/PostprocAbiVersion) — skipping"
                   << std::endl;
        dlclose(handle);
        return;
    }

    const std::string plugin_version = version_fn();
    if (plugin_version != postproc_abi::kAbiVersion) {
        std::cerr << "[PostprocRegistry] " << path
                   << " ABI version mismatch (plugin=" << plugin_version
                   << " server=" << postproc_abi::kAbiVersion << ") — skipping"
                   << std::endl;
        dlclose(handle);
        return;
    }

    postproc_abi::IPostprocess* instance = create_fn();
    if (!instance) {
        std::cerr << "[PostprocRegistry] " << path
                   << " CreatePostprocess() returned null — skipping" << std::endl;
        dlclose(handle);
        return;
    }

    const postproc_abi::PluginDescription abi_desc = instance->pluginInfo();

    const std::string name = abi_desc.name;
    if (find(name) != nullptr) {
        std::cerr << "[PostprocRegistry] " << path
                   << " declares name \"" << name
                   << "\" which is already registered — skipping "
                      "(built-ins and earlier-loaded plugins are never shadowed)"
                   << std::endl;
        destroy_fn(instance);
        dlclose(handle);
        return;
    }

    PostprocessInfo info;
    info.name        = name;
    info.description = abi_desc.description;
    for (const auto& layout : abi_desc.layouts) {
        std::vector<TensorExpectation> converted_layout;
        converted_layout.reserve(layout.size());
        for (const auto& expect : layout) converted_layout.push_back(postproc_convert::fromAbi(expect));
        info.layouts.push_back(std::move(converted_layout));
    }
    for (const auto& param : abi_desc.parameters) {
        info.parameters.push_back(postproc_convert::fromAbi(param));
    }
    info.instance.reset(new PostprocPlugin(instance, destroy_fn));

    std::cout << "[PostprocRegistry] loaded plugin \"" << name << "\" from " << path
              << std::endl;
    registerPostprocess(std::move(info));

    // Deliberately not dlclose(handle) — this plugin's vtable and code live
    // inside this handle for as long as the registry entry does, which is
    // the process lifetime.
}

void PostprocRegistry::loadDirectory(const std::string& dir) {
    const std::vector<std::string> paths = listSharedObjects(dir);
    if (paths.empty()) {
        std::cout << "[PostprocRegistry] no plugins found in " << dir << std::endl;
        return;
    }
    for (const auto& path : paths) loadOne(path);
}
