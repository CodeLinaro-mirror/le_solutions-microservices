// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// AiHubClient — C++ port of qai-hub-models-cli fetch.py
//
// Downloads pre-compiled model assets from the public AI Hub S3 bucket:
//   https://qaihub-public-assets.s3.us-west-2.amazonaws.com
//
// URL pattern (mirrors Python _asset_url()):
//   {STORE_URL}/qai-hub-models/models/{model_id}/releases/v{version}/
//     {model_id}-{runtime}-{precision}.zip
//   or (chipset-specific):
//     {model_id}-{runtime}-{precision}-{chipset_underscored}.zip
//
// No authentication required — the S3 bucket is public.
// ─────────────────────────────────────────────────────────────────────────────

#include <string>
#include <vector>
#include <functional>
#include <stdexcept>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// Exceptions
// ─────────────────────────────────────────────────────────────────────────────

struct AiHubAssetNotFound : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct AiHubDownloadError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ─────────────────────────────────────────────────────────────────────────────
// AiHubClient
// ─────────────────────────────────────────────────────────────────────────────
class AiHubClient {
public:
    // Public S3 store base URL
    static constexpr const char* STORE_URL =
        "https://qaihub-public-assets.s3.us-west-2.amazonaws.com";

    // PyPI JSON API for version discovery
    static constexpr const char* PYPI_URL =
        "https://pypi.org/pypi/qai-hub-models/json";

    // ── URL construction ──────────────────────────────────────────────────────

    /**
     * Build the S3 asset URL for a model.
     *
     * Mirrors Python _asset_url() in fetch.py.
     *
     * @param model_id   Model ID (e.g. "nomic_embed_text")
     * @param runtime    Target runtime (e.g. "qnn_dlc", "genie", "tflite")
     * @param precision  Model precision (e.g. "float", "w8a8")
     * @param version    AI Hub Models version (e.g. "0.45.0")
     * @param chipset    Optional chipset name (e.g. "qcs8550"). Empty = generic.
     * @return           Full S3 URL to the .zip asset
     */
    static std::string buildUrl(const std::string& model_id,
                                const std::string& runtime,
                                const std::string& precision,
                                const std::string& version,
                                const std::string& chipset = "");

    // ── Asset existence check ─────────────────────────────────────────────────

    /**
     * Check if an asset exists at the given URL via HTTP HEAD.
     *
     * @param url  S3 URL to check
     * @return     true if HTTP 200, false if 403/404
     * @throws     AiHubDownloadError on unexpected HTTP status or network error
     */
    static bool exists(const std::string& url);

    /**
     * Return Content-Length for an asset URL via HTTP HEAD.
     * Returns 0 if the server does not provide a positive length.
     */
    static int64_t contentLength(const std::string& url);

    /**
     * Resolve the best available URL for a model asset.
     *
     * Tries chipset-specific URL first (if chipset is non-empty), then falls
     * back to the generic URL. Mirrors Python get_asset_url() in fetch.py.
     *
     * @throws AiHubAssetNotFound if neither URL exists
     */
    static std::string resolveUrl(const std::string& model_id,
                                  const std::string& runtime,
                                  const std::string& precision,
                                  const std::string& version,
                                  const std::string& chipset = "");

    // ── Download ──────────────────────────────────────────────────────────────

    /**
     * Download a file from url to dest_path with resume support.
     *
     * Uses HTTP Range header to resume partial downloads. Retries up to
     * num_retries times on connection failure or incomplete download.
     *
     * @param url          URL to download
     * @param dest_path    Destination file path (must not exist)
     * @param progress_cb  Optional callback: (bytes_done, total_bytes)
     * @param num_retries  Number of retry attempts (default 4)
     * @throws AiHubDownloadError on HTTP error or incomplete download
     */
    static void download(const std::string& url,
                         const std::string& dest_path,
                         std::function<void(int64_t, int64_t)> progress_cb = {},
                         int num_retries = 4);

    // ── ZIP extraction ────────────────────────────────────────────────────────

    /**
     * Extract a ZIP archive to dest_dir.
     *
     * If the archive contains a single top-level directory, its contents are
     * unwrapped directly into dest_dir (mirrors Python extract_zip_file()).
     * If the archive contains multiple top-level entries, they are placed
     * into dest_dir as-is.
     *
     * @param zip_path  Path to the .zip file
     * @param dest_dir  Destination directory (must not exist)
     * @return          Path to the extracted directory
     * @throws          AiHubDownloadError on extraction failure
     */
    static std::string extractZip(const std::string& zip_path,
                                  const std::string& dest_dir);

    // ── Version discovery ─────────────────────────────────────────────────────

    /**
     * Fetch the list of published AI Hub Models versions from PyPI.
     *
     * Returns versions sorted newest-first.
     *
     * @throws AiHubDownloadError on network or parse error
     */
    static std::vector<std::string> fetchVersions();

private:
    // Replace hyphens with underscores in chipset name (mirrors Python)
    static std::string normalizeChipset(const std::string& chipset);
};
