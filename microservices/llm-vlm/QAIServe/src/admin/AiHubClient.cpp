// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// AiHubClient — Implementation
//
// C++ port of qai-hub-models-cli fetch.py + utils.py
//
// HTTP: libcurl (already a transitive dependency via Drogon)
// ZIP:  libzip
// ─────────────────────────────────────────────────────────────────────────────

#include "admin/AiHubClient.h"

#include <curl/curl.h>
#include <zip.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <iostream>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// libcurl write callback — appends data to a std::string
size_t writeToString(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

// libcurl write callback — writes data to a std::ofstream
size_t writeToFile(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* f = static_cast<std::ofstream*>(userdata);
    f->write(ptr, static_cast<std::streamsize>(size * nmemb));
    return size * nmemb;
}

// libcurl progress callback — forwards to user-supplied function
struct ProgressData {
    std::function<void(int64_t, int64_t)> cb;
    int64_t offset; // bytes already downloaded before this session (resume)
};

int progressCallback(void* userdata, curl_off_t dltotal, curl_off_t dlnow,
                     curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* pd = static_cast<ProgressData*>(userdata);
    if (pd->cb && dltotal > 0) {
        pd->cb(pd->offset + dlnow, pd->offset + dltotal);
    }
    return 0; // returning non-zero aborts the transfer
}

// RAII wrapper for CURL handle
struct CurlHandle {
    CURL* h;
    explicit CurlHandle() : h(curl_easy_init()) {
        if (!h) throw AiHubDownloadError("curl_easy_init() failed");
    }
    ~CurlHandle() { if (h) curl_easy_cleanup(h); }
    CurlHandle(const CurlHandle&) = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;
};

// Perform a curl request and return the HTTP status code.
// Throws AiHubDownloadError on curl error.
long curlPerform(CURL* h) {
    CURLcode rc = curl_easy_perform(h);
    if (rc != CURLE_OK) {
        throw AiHubDownloadError(
            std::string("curl error: ") + curl_easy_strerror(rc));
    }
    long http_code = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &http_code);
    return http_code;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// normalizeChipset — replace hyphens with underscores (mirrors Python)
// ─────────────────────────────────────────────────────────────────────────────
std::string AiHubClient::normalizeChipset(const std::string& chipset) {
    std::string s = chipset;
    std::replace(s.begin(), s.end(), '-', '_');
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// buildUrl — construct S3 asset URL
//
// Mirrors Python _asset_url() in fetch.py:
//   folder = "qai-hub-models/models/{model_id}/releases/v{version}"
//   file   = "{model_id}-{runtime}-{precision}.zip"
//         or "{model_id}-{runtime}-{precision}-{chipset_underscored}.zip"
// ─────────────────────────────────────────────────────────────────────────────
std::string AiHubClient::buildUrl(const std::string& model_id,
                                   const std::string& runtime,
                                   const std::string& precision,
                                   const std::string& version,
                                   const std::string& chipset) {
    std::string folder = std::string("qai-hub-models/models/")
                       + model_id + "/releases/v" + version;

    std::string filename = model_id + "-" + runtime + "-" + precision;
    if (!chipset.empty()) {
        filename += "-" + normalizeChipset(chipset);
    }
    filename += ".zip";

    return std::string(STORE_URL) + "/" + folder + "/" + filename;
}

// ─────────────────────────────────────────────────────────────────────────────
// exists — HTTP HEAD request to check asset availability
// ─────────────────────────────────────────────────────────────────────────────
bool AiHubClient::exists(const std::string& url) {
    CurlHandle ch;
    CURL* h = ch.h;

    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_NOBODY, 1L);          // HEAD request
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);

    long code = curlPerform(h);

    if (code == 200) return true;
    if (code == 403 || code == 404) return false;

    throw AiHubDownloadError(
        "Unexpected HTTP " + std::to_string(code) + " checking " + url);
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveUrl — try chipset URL first, fall back to generic
// ─────────────────────────────────────────────────────────────────────────────
std::string AiHubClient::resolveUrl(const std::string& model_id,
                                     const std::string& runtime,
                                     const std::string& precision,
                                     const std::string& version,
                                     const std::string& chipset) {
    if (!chipset.empty()) {
        std::string chipset_url = buildUrl(model_id, runtime, precision, version, chipset);
        if (exists(chipset_url)) return chipset_url;
    }

    std::string generic_url = buildUrl(model_id, runtime, precision, version);
    if (exists(generic_url)) return generic_url;

    std::string msg = "No asset found for model='" + model_id
                    + "', runtime='" + runtime
                    + "', precision='" + precision
                    + "', version='" + version + "'";
    if (!chipset.empty()) msg += ", chipset='" + chipset + "'";
    msg += ".\n  Browse available models: https://aihub.qualcomm.com/models";
    throw AiHubAssetNotFound(msg);
}

// ─────────────────────────────────────────────────────────────────────────────
// download — streaming GET with Range header (resumable)
// ─────────────────────────────────────────────────────────────────────────────
void AiHubClient::download(const std::string& url,
                            const std::string& dest_path,
                            std::function<void(int64_t, int64_t)> progress_cb,
                            int num_retries) {
    fs::path dest(dest_path);
    if (dest.has_parent_path()) {
        fs::create_directories(dest.parent_path());
    }

    for (int attempt = 0; attempt <= num_retries; ++attempt) {
        // Determine how many bytes we already have (for resume)
        int64_t bytes_so_far = 0;
        if (fs::exists(dest)) {
            bytes_so_far = static_cast<int64_t>(fs::file_size(dest));
        }

        CurlHandle ch;
        CURL* h = ch.h;

        // Open file in append mode if resuming, write mode otherwise
        std::ios::openmode mode = (bytes_so_far > 0)
            ? (std::ios::binary | std::ios::app)
            : (std::ios::binary | std::ios::trunc);
        std::ofstream file(dest_path, mode);
        if (!file) {
            throw AiHubDownloadError("Cannot open destination file: " + dest_path);
        }

        curl_easy_setopt(h, CURLOPT_URL, url.c_str());
        curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(h, CURLOPT_TIMEOUT, 0L);       // no timeout for large files
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, writeToFile);
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &file);

        // Resume from where we left off
        if (bytes_so_far > 0) {
            curl_easy_setopt(h, CURLOPT_RESUME_FROM_LARGE,
                             static_cast<curl_off_t>(bytes_so_far));
        }

        // Progress callback
        ProgressData pd{progress_cb, bytes_so_far};
        if (progress_cb) {
            curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, progressCallback);
            curl_easy_setopt(h, CURLOPT_XFERINFODATA, &pd);
        }

        long code = 0;
        try {
            code = curlPerform(h);
        } catch (const AiHubDownloadError& e) {
            file.close();
            if (attempt < num_retries) {
                std::cerr << "[AiHubClient] Download error (" << e.what()
                          << "), retrying (" << attempt + 1 << "/" << num_retries << ")...\n";
                continue;
            }
            throw;
        }

        file.close();

        if (code != 200 && code != 206) {
            // 206 = Partial Content (resume accepted)
            if (attempt < num_retries) {
                std::cerr << "[AiHubClient] HTTP " << code
                          << ", retrying (" << attempt + 1 << "/" << num_retries << ")...\n";
                continue;
            }
            throw AiHubDownloadError(
                "HTTP " + std::to_string(code) + " downloading " + url);
        }

        // Verify download completeness using Content-Length
        double content_length = 0;
        curl_easy_getinfo(h, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &content_length);
        int64_t actual = static_cast<int64_t>(fs::file_size(dest));
        int64_t expected = bytes_so_far + static_cast<int64_t>(content_length);

        if (content_length > 0 && actual < expected) {
            if (attempt < num_retries) {
                std::cerr << "[AiHubClient] Incomplete download (" << actual
                          << "/" << expected << " bytes), retrying ("
                          << attempt + 1 << "/" << num_retries << ")...\n";
                continue;
            }
            throw AiHubDownloadError(
                "Incomplete download after " + std::to_string(num_retries)
                + " retries: got " + std::to_string(actual)
                + "/" + std::to_string(expected) + " bytes");
        }

        return; // success
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// extractZip — extract ZIP to dest_dir, unwrap single top-level dir
//
// Mirrors Python extract_zip_file() in utils.py:
//   - If the archive has a single top-level directory, its contents are
//     moved directly into dest_dir (the wrapper dir is stripped).
//   - If the archive has multiple top-level entries, they go into dest_dir.
// ─────────────────────────────────────────────────────────────────────────────
std::string AiHubClient::extractZip(const std::string& zip_path,
                                     const std::string& dest_dir) {
    if (fs::exists(dest_dir)) {
        throw AiHubDownloadError(
            "Destination already exists: " + dest_dir);
    }

    int err = 0;
    zip_t* za = zip_open(zip_path.c_str(), ZIP_RDONLY, &err);
    if (!za) {
        zip_error_t ze;
        zip_error_init_with_code(&ze, err);
        std::string msg = "Cannot open ZIP: " + zip_path
                        + " (" + zip_error_strerror(&ze) + ")";
        zip_error_fini(&ze);
        throw AiHubDownloadError(msg);
    }

    // Extract to a temporary directory first
    fs::path tmp_dir = fs::path(dest_dir).parent_path()
                     / (fs::path(dest_dir).filename().string() + ".tmp");
    fs::create_directories(tmp_dir);

    zip_int64_t num_entries = zip_get_num_entries(za, 0);
    for (zip_int64_t i = 0; i < num_entries; ++i) {
        const char* name = zip_get_name(za, i, 0);
        if (!name) continue;

        fs::path entry_path = tmp_dir / name;

        // Security: ensure no path traversal
        auto canonical_tmp = fs::weakly_canonical(tmp_dir);
        auto canonical_entry = fs::weakly_canonical(entry_path);
        if (canonical_entry.string().find(canonical_tmp.string()) != 0) {
            zip_close(za);
            fs::remove_all(tmp_dir);
            throw AiHubDownloadError(
                "Unsafe ZIP entry detected: " + std::string(name));
        }

        // Directory entry
        if (name[strlen(name) - 1] == '/') {
            fs::create_directories(entry_path);
            continue;
        }

        // File entry
        fs::create_directories(entry_path.parent_path());

        zip_file_t* zf = zip_fopen_index(za, i, 0);
        if (!zf) {
            zip_close(za);
            fs::remove_all(tmp_dir);
            throw AiHubDownloadError(
                "Cannot open ZIP entry: " + std::string(name));
        }

        std::ofstream out(entry_path, std::ios::binary);
        char buf[65536];
        zip_int64_t n;
        while ((n = zip_fread(zf, buf, sizeof(buf))) > 0) {
            out.write(buf, n);
        }
        zip_fclose(zf);
    }
    zip_close(za);

    // Check if there is a single top-level directory (unwrap it)
    std::vector<fs::path> top_level;
    for (const auto& e : fs::directory_iterator(tmp_dir)) {
        top_level.push_back(e.path());
    }

    fs::path final_dest(dest_dir);
    fs::create_directories(final_dest.parent_path());

    if (top_level.size() == 1 && fs::is_directory(top_level[0])) {
        // Unwrap: move the single top-level dir to dest_dir
        fs::rename(top_level[0], final_dest);
        fs::remove_all(tmp_dir);
    } else {
        // Multiple entries: move the whole tmp_dir to dest_dir
        fs::rename(tmp_dir, final_dest);
    }

    return final_dest.string();
}

// ─────────────────────────────────────────────────────────────────────────────
// fetchVersions — query PyPI for published qai-hub-models versions
// ─────────────────────────────────────────────────────────────────────────────
std::vector<std::string> AiHubClient::fetchVersions() {
    CurlHandle ch;
    CURL* h = ch.h;

    std::string response_body;
    curl_easy_setopt(h, CURLOPT_URL, PYPI_URL);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &response_body);

    long code = curlPerform(h);
    if (code != 200) {
        throw AiHubDownloadError(
            "PyPI returned HTTP " + std::to_string(code));
    }

    json pypi = json::parse(response_body);
    auto releases = pypi.value("releases", json::object());

    std::vector<std::string> versions;
    versions.reserve(releases.size());
    for (const auto& [ver, _] : releases.items()) {
        versions.push_back(ver);
    }

    // Sort newest-first (lexicographic descending works for semver X.Y.Z)
    std::sort(versions.begin(), versions.end(), std::greater<std::string>());
    return versions;
}
