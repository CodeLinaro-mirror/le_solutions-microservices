// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// HfDirectClient — Implementation
//
// Downloads model files directly from HuggingFace for formats not supported
// by the GenieX SDK (e.g. .task, .tflite, .litertlm).
//
// HTTP: libcurl (same as AiHubClient)
// JSON: nlohmann/json
// ─────────────────────────────────────────────────────────────────────────────

#include "admin/HfDirectClient.h"
#include "admin/AiHubClient.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <iostream>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// libcurl write callback — appends HTTP response body to a std::string
size_t writeToString(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

// Return the lowercased file extension including the dot, e.g. ".task"
std::string fileExtension(const std::string& filename) {
    auto pos = filename.rfind('.');
    if (pos == std::string::npos) return "";
    std::string ext = filename.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// HfDirectClient::isSupportedModelFile
// ─────────────────────────────────────────────────────────────────────────────

bool HfDirectClient::isSupportedModelFile(const std::string& filename) {
    const std::string ext = fileExtension(filename);
    return ext == ".task" || ext == ".tflite" || ext == ".litertlm";
}

// ─────────────────────────────────────────────────────────────────────────────
// HfDirectClient::listFiles
// ─────────────────────────────────────────────────────────────────────────────

std::vector<HfDirectClient::FileInfo> HfDirectClient::listFiles(
        const std::string& repo) {
    // GET https://huggingface.co/api/models/{org}/{repo}
    std::string url = std::string(HF_API_URL) + "/" + repo;

    CURL* curl = curl_easy_init();
    if (!curl) throw HfDirectError("HfDirectClient: failed to init curl");

    std::string response_body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    // User-Agent to avoid HF rate-limiting on bare curl
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "qaiserve/1.0");

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw HfDirectError(
            std::string("HfDirectClient: curl error querying ") + repo + ": " +
            curl_easy_strerror(res));
    }
    if (http_code == 404) {
        throw HfDirectError(
            std::string("HfDirectClient: repo not found on HuggingFace: ") + repo);
    }
    if (http_code != 200) {
        throw HfDirectError(
            std::string("HfDirectClient: HTTP ") + std::to_string(http_code) +
            " querying " + repo);
    }

    // Parse JSON response — extract siblings[].rfilename and .size
    std::vector<FileInfo> files;
    try {
        json root = json::parse(response_body);
        for (const auto& sibling : root.value("siblings", json::array())) {
            FileInfo fi;
            fi.name = sibling.value("rfilename", "");
            fi.size = sibling.value("size", int64_t(0));
            if (!fi.name.empty()) files.push_back(std::move(fi));
        }
    } catch (const std::exception& e) {
        throw HfDirectError(
            std::string("HfDirectClient: failed to parse HF API response for ") +
            repo + ": " + e.what());
    }

    return files;
}

// ─────────────────────────────────────────────────────────────────────────────
// HfDirectClient::pull
// ─────────────────────────────────────────────────────────────────────────────

std::string HfDirectClient::pull(
        const std::string& repo,
        const std::string& dest_dir,
        std::function<void(int64_t, int64_t)> progress_cb) {

    // 1. Query file list
    auto all_files = listFiles(repo);

    // 2. Filter to supported model files
    std::vector<FileInfo> model_files;
    for (const auto& fi : all_files) {
        if (isSupportedModelFile(fi.name)) model_files.push_back(fi);
    }
    if (model_files.empty()) {
        throw HfDirectError(
            std::string("HfDirectClient: no supported model files (.task/.tflite/.litertlm) "
                        "found in repo: ") + repo);
    }

    // 3. Create destination directory: dest_dir/{org}/{repo-leaf}/
    fs::path install_dir = fs::path(dest_dir) / repo;
    {
        std::error_code ec;

        // If bundle already exists, check if it is complete.
        // - Complete (hf_manifest.json present): return early (cached).
        // - Incomplete (no manifest): remove and re-download cleanly.
        if (fs::exists(install_dir, ec)) {
            bool complete = fs::exists(install_dir / "hf_manifest.json");
            if (complete) {
                return install_dir.string();  // already complete, use cached
            }
            // Incomplete bundle from a previous interrupted download — clean up
            fs::remove_all(install_dir, ec);
            if (ec) {
                throw HfDirectError(
                    std::string("HfDirectClient: failed to remove incomplete bundle ") +
                    install_dir.string() + ": " + ec.message());
            }
            std::cout << "[HfDirectClient] Removed incomplete bundle for re-download: "
                      << install_dir.string() << "\n";
        }

        fs::create_directories(install_dir, ec);
        if (ec) {
            throw HfDirectError(
                std::string("HfDirectClient: failed to create directory ") +
                install_dir.string() + ": " + ec.message());
        }
        // Remove any stale .inflight/ sentinel left by a previous failed GenieX
        // pull on the same repo. Without this, AiHubClient::download() resumes
        // from the wrong offset and gets HTTP 416.
        fs::path inflight = install_dir / ".inflight";
        if (fs::exists(inflight, ec)) {
            fs::remove_all(inflight, ec);
        }
    }

    // 4. Compute total bytes for progress reporting
    int64_t total_bytes = 0;
    for (const auto& fi : model_files) total_bytes += fi.size;

    // 5. Download each file
    int64_t bytes_so_far = 0;
    std::string primary_file;  // first downloaded file → config_file

    for (const auto& fi : model_files) {
        std::string url = std::string(HF_CDN_URL) + "/" + repo +
                          "/resolve/main/" + fi.name;
        fs::path dest_path = install_dir / fi.name;

        // Remove any stale partial file so AiHubClient::download() starts fresh
        // instead of attempting a range-resume that may return HTTP 416.
        {
            std::error_code ec;
            if (fs::exists(dest_path, ec)) {
                fs::remove(dest_path, ec);
            }
        }

        // Progress wrapper: accumulate bytes across files
        int64_t file_offset = bytes_so_far;
        std::function<void(int64_t, int64_t)> file_progress;
        if (progress_cb) {
            file_progress = [&progress_cb, &bytes_so_far, file_offset, total_bytes](
                    int64_t done, int64_t /*file_total*/) {
                progress_cb(file_offset + done, total_bytes);
            };
        }

        std::cout << "[HfDirectClient] Downloading " << fi.name << " from " << repo << "\n";
        AiHubClient::download(url, dest_path.string(), file_progress);
        bytes_so_far += fi.size;

        if (primary_file.empty()) primary_file = fi.name;
    }

    // Signal completion
    if (progress_cb) progress_cb(total_bytes, total_bytes);

    // 6. Determine PluginId from file extension
    std::string plugin_id;
    {
        const std::string ext = fileExtension(primary_file);
        if (ext == ".task" || ext == ".tflite" || ext == ".litertlm") {
            plugin_id = "litert_lm";
        } else {
            plugin_id = "litert_lm";  // safe default for any future extensions
        }
    }

    // 7. Derive ModelName: leaf part of repo (after last '/')
    std::string model_name = repo;
    {
        auto slash = repo.rfind('/');
        if (slash != std::string::npos) model_name = repo.substr(slash + 1);
    }

    // 8. Build ModelFile map from downloaded files
    json model_file_json = json::object();
    for (size_t i = 0; i < model_files.size(); ++i) {
        // Use quantization key "default" for single file, or index for multiple
        std::string key = (model_files.size() == 1) ? "default"
                          : ("variant_" + std::to_string(i));
        model_file_json[key] = {
            {"Name",       model_files[i].name},
            {"Downloaded", true},
            {"Size",       model_files[i].size},
        };
    }

    // 9. Write hf_manifest.json (geniex.json-compatible schema)
    json manifest = {
        {"Name",      repo},
        {"ModelName", model_name},
        {"ModelType", "llm"},
        {"PluginId",  plugin_id},
        {"ModelFile", model_file_json},
        {"MMProjFile",    {{"Name",""},{"Downloaded",false},{"Size",0}}},
        {"TokenizerFile", {{"Name",""},{"Downloaded",false},{"Size",0}}},
        {"ExtraFiles", json::array()},
    };

    fs::path manifest_path = install_dir / "hf_manifest.json";
    {
        std::ofstream mf(manifest_path);
        if (!mf) {
            throw HfDirectError(
                std::string("HfDirectClient: failed to write hf_manifest.json at ") +
                manifest_path.string());
        }
        mf << manifest.dump(2) << "\n";
    }

    std::cout << "[HfDirectClient] Installed " << repo << " → " << install_dir.string() << "\n";
    return install_dir.string();
}
