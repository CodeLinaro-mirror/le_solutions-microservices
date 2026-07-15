// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// GenieXClient — C++ wrapper around the GenieX SDK model-manager C ABI
//
// The GenieX SDK (libgeniex.so, implemented in Rust) exposes a C ABI declared
// in <geniex_model.h>. Unlike AiHubClient — which is a from-scratch C++ port of
// the qai-hub-models S3 download logic — this class does NOT reimplement any
// download logic; it delegates to the native library, which can pull from
// multiple hubs (HuggingFace, AI Hub, ModelScope, local fs).
//
// Model names are "org/repo", "org/repo:quant", or a short alias
// (e.g. "qwen3"). AI-Hub-style repos such as "ai-hub-models/Qwen3-4B-..."
// resolve to the AI Hub source automatically.
//
// This mirrors the AiHubClient surface (static methods, download-error
// exception) so AdminController can branch between the two symmetrically.
// ─────────────────────────────────────────────────────────────────────────────

#include <string>
#include <functional>
#include <stdexcept>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// Exceptions
// ─────────────────────────────────────────────────────────────────────────────

struct GenieXDownloadError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ─────────────────────────────────────────────────────────────────────────────
// GenieXClient
// ─────────────────────────────────────────────────────────────────────────────
class GenieXClient {
public:
    // Hub source selector — mirrors geniex_HubSource in <geniex_model.h>.
    // Kept as a plain enum here so callers (AdminController) don't need the
    // native header. Values MUST match the SDK enum.
    enum class Hub : int {
        Auto        = 0,
        HuggingFace = 1,
        ModelScope  = 2,
        AiHub       = 3,
        Volces      = 4,
        LocalFs     = 127,
    };

    // Resolved on-disk paths for a pulled model (subset of geniex_ModelPaths).
    struct ModelPaths {
        std::string model_path;     // Main model file (absolute)
        std::string model_dir;      // Model directory (always set)
        std::string model_name;     // Architecture name, e.g. "qwen3-4b"
        std::string plugin_id;      // Plugin ID / runtime, e.g. "qairt", "llama_cpp"
        std::string mmproj_path;    // Multimodal projection file (VLM only, may be empty)
        std::string tokenizer_path; // Tokenizer file (may be empty)
        bool        is_vlm = false; // true if the SDK reports model_type == VLM
    };

    /**
     * Map a hub string ("auto", "hf"/"huggingface", "aihub", "modelscope",
     * "volces", "local"/"localfs") to a Hub enum value.
     * Defaults to Hub::Auto for unknown/empty input.
     */
    static Hub parseHub(const std::string& hub);

    /**
     * Download a model into the local cache (blocking, resumable).
     *
     * Delegates to geniex_model_pull(). Ensures the model manager is
     * initialised once (geniex_model_init) against data_dir before the first
     * pull.
     *
     * @param model_name  "org/repo", "org/repo:quant", or a short alias.
     * @param precision   Optional quantization hint (e.g. "Q4_K_M"). Empty = auto.
     * @param hub         Source hub (Hub::Auto lets the SDK decide).
     * @param chipset     Optional AI Hub target chipset. Empty = auto-detect
     *                    (works on the QCS9075 board).
     * @param data_dir    Cache directory to init the manager with. Empty falls
     *                    back to GENIEX_DATADIR env / ~/.cache/geniex.
     * @param progress_cb Optional callback (bytes_done, total_bytes) summed
     *                    across all files in the pull.
     * @throws GenieXDownloadError on any negative geniex_ErrorCode, carrying
     *         geniex_model_last_error_message().
     */
    static void pull(const std::string& model_name,
                     const std::string& precision,
                     Hub hub,
                     const std::string& chipset,
                     const std::string& data_dir,
                     std::function<void(int64_t, int64_t)> progress_cb = {});

    /**
     * Resolve the on-disk paths for a cached model.
     * Wraps geniex_model_get_paths() + geniex_model_paths_free().
     *
     * @param model_name  "org/repo" or "org/repo:quant".
     * @throws GenieXDownloadError if the model is not cached / on error.
     */
    static ModelPaths getPaths(const std::string& model_name);

private:
    // Initialise the native model manager exactly once (thread-safe).
    static void ensureInit(const std::string& data_dir);
};
