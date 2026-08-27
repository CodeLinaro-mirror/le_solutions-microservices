// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// HfDirectClient — Direct HuggingFace download for model formats not supported
// by the GenieX SDK (e.g. .task, .tflite, .litertlm).
//
// Used as a fallback when GenieXClient::pull() fails with a "no recognizable
// model files" error — i.e. the repo exists on HF but contains file types that
// the GenieX model-manager does not handle.
//
// On success, writes a geniex.json-compatible manifest (hf_manifest.json) next
// to the downloaded files so ModelConfigManager can register the model via the
// existing parseGenieXJson path.
// ─────────────────────────────────────────────────────────────────────────────

#include <string>
#include <vector>
#include <functional>
#include <stdexcept>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// Exceptions
// ─────────────────────────────────────────────────────────────────────────────

struct HfDirectError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ─────────────────────────────────────────────────────────────────────────────
// HfDirectClient
// ─────────────────────────────────────────────────────────────────────────────

class HfDirectClient {
public:
    // HuggingFace API and CDN base URLs
    static constexpr const char* HF_API_URL  = "https://huggingface.co/api/models";
    static constexpr const char* HF_CDN_URL  = "https://huggingface.co";

    struct FileInfo {
        std::string name;
        int64_t     size{0};
    };

    /**
     * Query the HF model API and return the list of files in the repo.
     * Throws HfDirectError on network failure or if the repo is not found.
     */
    static std::vector<FileInfo> listFiles(const std::string& repo);

    /**
     * Returns true if the filename has a model-file extension that we can
     * register with the litert_lm runtime (.task, .tflite, .litertlm).
     */
    static bool isSupportedModelFile(const std::string& filename);

    /**
     * Download all supported model files from `repo` into
     * `dest_dir/{org}/{repo}/`, then write `hf_manifest.json`.
     *
     * progress_cb(bytes_done, total_bytes) is called periodically.
     *
     * Returns the absolute path to the installed directory.
     * Throws HfDirectError when no supported files are found or on download
     * failure.
     */
    static std::string pull(
        const std::string& repo,
        const std::string& dest_dir,
        std::function<void(int64_t, int64_t)> progress_cb = {});
};
