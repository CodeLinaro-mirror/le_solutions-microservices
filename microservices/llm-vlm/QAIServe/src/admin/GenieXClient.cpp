// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// GenieXClient — Implementation
//
// Thin C++ wrapper over the GenieX SDK model-manager C ABI (<geniex_model.h>,
// backed by libgeniex.so). All heavy lifting (multi-hub resolution, download,
// resume, manifest writing) happens inside the native library.
// ─────────────────────────────────────────────────────────────────────────────

#include "admin/GenieXClient.h"

#include <geniex_model.h>   // geniex_model_* C ABI (staged into /usr/include by the Dockerfile)

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>

namespace {

// Progress trampoline: the SDK reports per-file progress; the job model tracks
// a single (bytes_done, total_bytes) pair, so we sum across all files.
// The user callback pointer is forwarded via geniex_ModelPullInput.user_data.
bool progressTrampoline(const geniex_FileProgress* files, int32_t count, void* user_data) {
    auto* cb = static_cast<std::function<void(int64_t, int64_t)>*>(user_data);
    if (!cb || !*cb) return true;  // continue; nothing to report to

    int64_t done = 0;
    int64_t total = 0;
    bool total_known = false;
    for (int32_t i = 0; i < count; ++i) {
        done += files[i].downloaded_bytes;
        if (files[i].total_bytes >= 0) {
            total += files[i].total_bytes;
            total_known = true;
        }
    }
    (*cb)(done, total_known ? total : 0);
    return true;  // returning false would cancel the download
}

// Build a GenieXDownloadError carrying the SDK's thread-local error message.
GenieXDownloadError makeError(const std::string& context) {
    const char* msg = geniex_model_last_error_message();
    std::string detail = msg ? msg : "unknown error";
    return GenieXDownloadError(context + ": " + detail);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// ensureInit — initialise the native model manager exactly once
//
// geniex_model_init() is documented as call-once-per-process; a second call is
// a no-op that returns GENIEX_ERROR_COMMON_INVALID_INPUT. We guard with a
// std::once_flag so concurrent fetch jobs can't race the first call.
// ─────────────────────────────────────────────────────────────────────────────
void GenieXClient::ensureInit(const std::string& data_dir) {
    static std::once_flag init_flag;
    static int32_t init_rc = GENIEX_SUCCESS;

    std::call_once(init_flag, [&]() {
        const char* dir = data_dir.empty() ? nullptr : data_dir.c_str();
        init_rc = geniex_model_init(dir);
    });

    if (init_rc != GENIEX_SUCCESS) {
        throw makeError("geniex_model_init failed");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// parseHub
// ─────────────────────────────────────────────────────────────────────────────
GenieXClient::Hub GenieXClient::parseHub(const std::string& hub) {
    std::string k;
    k.reserve(hub.size());
    for (char c : hub) k.push_back(static_cast<char>(std::tolower(c)));

    if (k == "hf" || k == "huggingface") return Hub::HuggingFace;
    if (k == "aihub")                    return Hub::AiHub;
    if (k == "modelscope")               return Hub::ModelScope;
    if (k == "volces")                   return Hub::Volces;
    if (k == "local" || k == "localfs")  return Hub::LocalFs;
    return Hub::Auto;
}

// ─────────────────────────────────────────────────────────────────────────────
// pull — download a model via geniex_model_pull()
// ─────────────────────────────────────────────────────────────────────────────
void GenieXClient::pull(const std::string& model_name,
                        const std::string& precision,
                        Hub hub,
                        const std::string& chipset,
                        const std::string& data_dir,
                        std::function<void(int64_t, int64_t)> progress_cb) {
    ensureInit(data_dir);

    // struct_size is the ABI version gate — MUST equal sizeof at the call site.
    geniex_ModelPullInput in = {};
    in.struct_size = static_cast<uint32_t>(sizeof(geniex_ModelPullInput));
    in.model_name  = model_name.c_str();
    in.quant       = precision.empty() ? nullptr : precision.c_str();
    in.hub         = static_cast<geniex_HubSource>(static_cast<int>(hub));
    in.local_path  = nullptr;
    in.hf_token    = nullptr;  // falls back to GENIEX_HFTOKEN env
    in.chipset     = chipset.empty() ? nullptr : chipset.c_str();
    in.display_name = nullptr; // derived from qualcomm/ or ai-hub-models/ repo
    in.model_type  = GENIEX_MODEL_TYPE_AUTO;

    // Forward the user callback through user_data; only wire the trampoline if
    // a callback was provided (NULL on_progress suppresses SDK progress).
    if (progress_cb) {
        in.on_progress = &progressTrampoline;
        in.user_data   = &progress_cb;
    } else {
        in.on_progress = nullptr;
        in.user_data   = nullptr;
    }

    int32_t rc = geniex_model_pull(&in);
    if (rc != GENIEX_SUCCESS) {
        throw makeError("geniex_model_pull failed for '" + model_name + "'");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// getPaths — resolve on-disk paths for a cached model
// ─────────────────────────────────────────────────────────────────────────────
GenieXClient::ModelPaths GenieXClient::getPaths(const std::string& model_name) {
    geniex_ModelPaths out = {};
    int32_t rc = geniex_model_get_paths(model_name.c_str(), &out);
    if (rc != GENIEX_SUCCESS) {
        throw makeError("geniex_model_get_paths failed for '" + model_name + "'");
    }

    ModelPaths paths;
    if (out.model_path)     paths.model_path     = out.model_path;
    if (out.model_dir)      paths.model_dir      = out.model_dir;
    if (out.model_name)     paths.model_name     = out.model_name;
    if (out.plugin_id)      paths.plugin_id      = out.plugin_id;
    if (out.mmproj_path)    paths.mmproj_path    = out.mmproj_path;
    if (out.tokenizer_path) paths.tokenizer_path = out.tokenizer_path;
    paths.is_vlm = (out.model_type == GENIEX_MODEL_TYPE_VLM);

    geniex_model_paths_free(&out);
    return paths;
}
