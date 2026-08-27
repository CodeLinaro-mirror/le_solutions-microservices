// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// AdminController — Implementation
//
// All endpoints are admin-only (X-Admin-Token header required).
// Model downloads run in background threads via ModelFetchJob.
// ─────────────────────────────────────────────────────────────────────────────

#include "controllers/AdminController.h"
#include "admin/AiHubClient.h"
#include "admin/DiskSpaceGuard.h"
#include "admin/GenieXClient.h"
#include "admin/HfDirectClient.h"
#include "admin/ModelAssetLock.h"
#include "admin/ModelFetchJob.h"
#include "qai_forge/managers/ModelConfigManager.h"

#include <drogon/HttpResponse.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <set>
#include <vector>

namespace fs = std::filesystem;
// Note: ModelConfigManager.h already declares `using json = nlohmann::ordered_json`
// at file scope; do not redeclare here to avoid a conflicting-declaration error.

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

constexpr size_t kDefaultMaxConcurrentDownloads = 3;
HttpResponsePtr jsonResp(const json& body, HttpStatusCode code = k200OK) {
    auto resp = HttpResponse::newHttpResponse();
    resp->setStatusCode(code);
    resp->setContentTypeCode(CT_APPLICATION_JSON);
    resp->setBody(body.dump());
    return resp;
}

HttpResponsePtr errorResp(const std::string& msg, HttpStatusCode code) {
    return jsonResp({{"error", msg}}, code);
}

// Read models directory from environment (same as ModelConfigManager)
std::string modelsDir() {
    const char* env = std::getenv("GENAI_MODELS_DIR");
    return env ? std::string(env) : "/mnt/work/models";
}

// Temp directory for ZIP downloads
std::string tmpDownloadDir() {
    const char* env = std::getenv("QAISERVE_TMP_DIR");
    return env ? std::string(env) : "/tmp/qaiserve-downloads";
}

size_t maxConcurrentDownloads() {
    const char* env = std::getenv("QAISERVE_MAX_CONCURRENT_DOWNLOADS");
    if (!env || env[0] == '\0') return kDefaultMaxConcurrentDownloads;
    try {
        size_t parsed = static_cast<size_t>(std::stoul(env));
        return parsed == 0 ? kDefaultMaxConcurrentDownloads : parsed;
    } catch (...) {
        LOG_WARN << "[AdminController] Invalid QAISERVE_MAX_CONCURRENT_DOWNLOADS='"
                 << env << "', defaulting to " << kDefaultMaxConcurrentDownloads;
        return kDefaultMaxConcurrentDownloads;
    }
}

std::string assetRuntime(const std::string& runtime);

bool hasWhitespace(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c);
    });
}

bool hasUnsafePathSegment(const std::string& value) {
    if (value.find('\\') != std::string::npos) return true;

    fs::path path(value);
    if (path.is_absolute()) return true;
    for (const auto& part : path) {
        if (part == "." || part == "..") return true;
    }
    return false;
}

bool isPathInsideDirectory(const fs::path& candidate, const fs::path& root) {
    fs::path c = candidate.lexically_normal();
    fs::path r = root.lexically_normal();
    if (c == r) return false;

    fs::path rel = c.lexically_relative(r);
    if (rel.empty()) return false;
    auto first = rel.begin();
    return first != rel.end() && *first != "..";
}

bool isPathAtOrUnder(const fs::path& candidate, const fs::path& root) {
    fs::path c = candidate.lexically_normal();
    fs::path r = root.lexically_normal();
    if (c == r) return true;

    fs::path rel = c.lexically_relative(r);
    if (rel.empty()) return false;
    auto first = rel.begin();
    return first != rel.end() && *first != "..";
}

std::string findRegisteredModelIdByPath(const fs::path& install_path,
                                        const std::string& preferred_id,
                                        const std::string& precision_hint = "") {
    // Normalize precision hint for case-insensitive substring match.
    std::string prec_norm = precision_hint;
    std::transform(prec_norm.begin(), prec_norm.end(), prec_norm.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    for (auto& c : prec_norm) if (c == '-') c = '_';

    for (int i = 0; i < 3; ++i) {
        if (i) std::this_thread::sleep_for(std::chrono::milliseconds(200));

        if (!preferred_id.empty() &&
            ModelConfigManager::getInstance().getModelConfig(preferred_id)) {
            return preferred_id;
        }

        if (!install_path.empty()) {
            auto models = ModelConfigManager::getInstance().getAvailableModels();
            // Two-pass: first try models whose id contains the precision hint,
            // then fall back to any match. This avoids returning the wrong
            // precision for multi-precision GGUF bundles (same bundle_path).
            for (int pass = 0; pass < 2; ++pass) {
                for (const auto& config : models) {
                    // Precision filter on first pass only.
                    if (pass == 0 && !prec_norm.empty()) {
                        std::string id_norm = config.id;
                        std::transform(id_norm.begin(), id_norm.end(), id_norm.begin(),
                                       [](unsigned char c){ return std::tolower(c); });
                        for (auto& c : id_norm) if (c == '-') c = '_';
                        if (id_norm.find(prec_norm) == std::string::npos) continue;
                    }
                    // Primary: match against bundle_path.
                    if (!config.bundle_path.empty()) {
                        fs::path bp(config.bundle_path);
                        if (bp == install_path.lexically_normal() ||
                            isPathAtOrUnder(bp, install_path)) {
                            return config.id;
                        }
                    }
                    // Fallback: match config_file (legacy bundles without bundle_path).
                    if (!config.config_file.empty()) {
                        fs::path config_path(config.config_file);
                        if (isPathAtOrUnder(config_path, install_path)) return config.id;
                    }
                }
                if (prec_norm.empty()) break;  // no precision hint → single pass
            }
        }
    }
    return "";
}

void requireRegisteredModelId(const std::string& model_id) {
    for (int i = 0; i < 3; ++i) {
        if (i) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (ModelConfigManager::getInstance().getModelConfig(model_id)) return;
    }
    throw std::runtime_error(
        "Model downloaded but not registered in registry: " + model_id);
}

json modelSummaryJson(const ModelConfig& m, bool include_config_file = false) {
    json entry = {
        {"id",         m.id},
        {"model_name", m.display_name},
        {"runtime",    m.runtime},
        {"model_type", m.model_type},
        {"memory_mb",  m.memory_requirement_mb},
    };
    if (m.model_type == "generative") {
        entry["supports_vision"]    = m.supports_vision;
        entry["supports_thinking"]  = m.supports_thinking;
        entry["supports_streaming"] = m.supports_streaming;
        entry["context_size"]       = m.context_size;
    }
    if (include_config_file) entry["config_file"] = m.config_file;
    return entry;
}

// AI Hub publishes assets under format-specific runtime tags rather than the
// logical backend name — "qnn" assets are named "qnn_context_binary" and
// "snpe" assets are named "qnn_dlc" (mirrors BackendFactory's acceptance of
// both spellings for backend selection). Translate the logical name to the
// asset tag before building the S3 URL / filenames; unrecognized runtimes
// pass through unchanged.
std::string assetRuntime(const std::string& runtime) {
    if (runtime == "qnn")  return "qnn_context_binary";
    if (runtime == "snpe") return "qnn_dlc";
    return runtime;
}

// Predictive AI backends (QNN, SNPE, LiteRT) accept either the logical
// runtime name or the AI-Hub asset tag it maps to (see assetRuntime() /
// BackendFactory) — metadata.json downloaded from AI Hub carries the asset
// tag (e.g. "qnn_dlc"), so both spellings must be recognized here.
bool isPredictiveRuntime(const std::string& runtime) {
    static const std::set<std::string> predictive_runtimes = {
        "qnn", "qnn_context_binary",
        "snpe", "qnn_dlc",
        "litert", "tflite",
    };
    return predictive_runtimes.count(runtime) > 0;
}

// AI Hub's published metadata.json has no "model_type" field — backfill it
// from "runtime" so ModelConfigManager (which defaults missing model_type to
// "generative") routes predictive bundles correctly. Inserted right after
// "model_name" to match the field ordering the rest of metadata.json uses.
void backfillModelType(const fs::path& meta_path) {
    json meta;
    try {
        std::ifstream in(meta_path);
        meta = json::parse(in);
    } catch (...) {
        return;
    }
    if (meta.contains("model_type")) return;

    std::string model_type = isPredictiveRuntime(meta.value("runtime", ""))
                                  ? "predictive" : "generative";

    json patched = json::object();
    bool inserted = false;
    for (auto it = meta.begin(); it != meta.end(); ++it) {
        patched[it.key()] = it.value();
        if (it.key() == "model_name") {
            patched["model_type"] = model_type;
            inserted = true;
        }
    }
    if (!inserted) patched["model_type"] = model_type;

    std::ofstream out(meta_path);
    out << patched.dump(4);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// checkAuth — validate X-Admin-Token header
// ─────────────────────────────────────────────────────────────────────────────
bool AdminController::checkAuth(const HttpRequestPtr& req,
                                 std::function<void(const HttpResponsePtr&)>& callback) {
    const char* expected = std::getenv("QAISERVE_ADMIN_TOKEN");
    if (!expected || expected[0] == '\0') {
        // No token configured — deny all admin requests
        callback(errorResp(
            "Admin access is disabled. Set QAISERVE_ADMIN_TOKEN to enable.",
            k403Forbidden));
        return false;
    }

    std::string token = req->getHeader("X-Admin-Token");
    if (token != expected) {
        callback(errorResp("Invalid or missing X-Admin-Token.", k403Forbidden));
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// POST /admin/models/fetch
// ─────────────────────────────────────────────────────────────────────────────
void AdminController::fetchModel(const HttpRequestPtr& req,
                                  std::function<void(const HttpResponsePtr&)>&& callback) {
    if (!checkAuth(req, callback)) return;

    // Parse request body
    json body;
    try {
        body = json::parse(req->getBody());
    } catch (...) {
        callback(errorResp("Invalid JSON body.", k400BadRequest));
        return;
    }

    std::string model;
    std::string runtime;
    std::string precision;
    std::string version;
    std::string chipset;
    std::string source;
    try {
        model     = body.value("model", "");
        runtime   = body.value("runtime", "");
        precision = body.value("precision", "float");
        version   = body.value("version", "");
        chipset   = body.value("chipset", "");
        source    = body.value("source", "aihub");
    } catch (const std::exception& e) {
        callback(errorResp(
            std::string("Request fields must be strings: ") + e.what(),
            k400BadRequest));
        return;
    }

    if (model.empty()) {
        callback(errorResp("'model' is required.", k400BadRequest));
        return;
    }
    if (hasWhitespace(model)) {
        callback(errorResp("'model' must not contain whitespace.", k400BadRequest));
        return;
    }
    if (hasUnsafePathSegment(model)) {
        callback(errorResp("'model' contains an unsafe path segment.", k400BadRequest));
        return;
    }
    if (source != "aihub" && source != "geniex") {
        callback(errorResp("'source' must be 'aihub' or 'geniex'.", k400BadRequest));
        return;
    }
    // 'runtime' is required only for the aihub S3 path (it is part of the asset
    // URL). GenieX resolves the runtime itself from the model repo.
    const std::string models_dir = modelsDir();

    if (source == "aihub" && runtime.empty()) {
        callback(errorResp("'runtime' is required.", k400BadRequest));
        return;
    }

    // If version not specified, fetch latest from PyPI (aihub path only —
    // GenieX does not use qai-hub-models package versions).
    if (source == "aihub" && version.empty()) {
        try {
            auto versions = AiHubClient::fetchVersions();
            if (versions.empty()) {
                callback(errorResp("Could not determine latest version from PyPI.", k502BadGateway));
                return;
            }
            version = versions.front();
        } catch (const std::exception& e) {
            callback(errorResp(
                std::string("Failed to fetch version list: ") + e.what(),
                k502BadGateway));
            return;
        }
    }

        // Reject up front if the destination bundle directory already exists —
    // extractZip() only checks this after the download completes, which
    // wastes a full download just to fail. Only applies to AI Hub where the
    // install path is deterministic. For GenieX, the SDK handles idempotency
    // internally (pull.rs merges manifests and skips already-downloaded files).
    if (source == "aihub") {
        std::string dest_path = admin::expectedInstallPath(
            models_dir, source, model, runtime, precision, chipset);
        if (fs::exists(dest_path)) {
            callback(errorResp(
                "Model already installed at " + dest_path
                    + " — delete it first (DELETE /admin/models/{model_id}) to re-fetch.",
                k409Conflict));
            return;
        }
    }

    admin::pruneStaleDownloadLocks(models_dir);

    fs::path lock_path = admin::downloadLockPath(
        models_dir, source, model, runtime, precision, chipset);
    std::string lock_error;
    auto& registry = ModelFetchJobRegistry::getInstance();
    registry.pruneOldJobs();
    const size_t max_active = maxConcurrentDownloads();
    size_t active_count = 0;
    auto job = registry.createIfBelowLimit(
        model, runtime, precision, version, chipset, source,
        max_active, active_count);
    if (!job) {
        callback(jsonResp({
            {"error", "Maximum concurrent model downloads reached."},
            {"active_downloads", active_count},
            {"max_concurrent_downloads", max_active},
        }, k409Conflict));
        return;
    }

    // Capture job_id for the response (job ptr is moved into the thread)
    std::string job_id = job->job_id;

    admin::LockJson existing_lock;
    if (!admin::createDownloadLock(
            lock_path,
            admin::buildNewDownloadLockJson(
                models_dir, job_id, source, model, runtime, precision, version,
                chipset, "pending"),
            lock_error,
            &existing_lock)) {
        job->setError(lock_error);
        job->status.store(FetchStatus::FAILED);
        json resp = {{"error", lock_error}};
        if (!existing_lock.empty()) {
            resp["details"] = {{"lock_file", lock_path.string()}, {"lock", existing_lock}};
        }
        callback(jsonResp(resp, k409Conflict));
        return;
    }

    // Launch background download thread. The lock is already created here; if
    // std::thread construction itself fails, remove it so future fetches are not
    // blocked by a stale pending lock.
    try {
        std::thread worker([job, model, runtime, precision, version, chipset,
                            source, lock_path, models_dir]() {
            const std::string tmp_dir    = tmpDownloadDir();
            const std::string asset_rt   = assetRuntime(runtime);

            try {
                if (source == "geniex") {
                    // ── GenieX path: delegate to the native SDK ────────────────
                    // The SDK resolves the hub (HuggingFace / AI Hub / …), downloads
                    // (with resume), and writes the bundle + geniex.json manifest
                    // under its data dir. We point that data dir at the scanned
                    // models directory so the hot-reload picks it up.
                    //
                    // Fallback: if the SDK rejects the repo because it contains file
                    // formats it doesn't recognise (e.g. .task/.tflite/.litertlm),
                    // we fall back to HfDirectClient which downloads directly from HF
                    // and writes a geniex.json-compatible hf_manifest.json.
                    job->status.store(FetchStatus::DOWNLOADING);
                    admin::requireMinimumFreeSpace(models_dir, "GENAI_MODELS_DIR");
                    std::string lock_error;
                    admin::writeJsonFile(
                        lock_path,
                        admin::buildUpdatedDownloadLockJson(
                            lock_path, models_dir, job->job_id, source, model,
                            runtime, precision, version, chipset, "downloading"),
                        lock_error);

                    bool use_hf_fallback = false;
                    try {
                        GenieXClient::pull(
                            model, precision,
                            GenieXClient::Hub::Auto,   // auto-select hub from repo name
                            chipset, models_dir,
                            [&job](int64_t done, int64_t total) {
                                job->bytes_downloaded.store(done);
                                job->total_bytes.store(total);
                            });
                    } catch (const GenieXDownloadError& e) {
                        std::string msg = e.what();
                        // GenieX rejects repos whose files it doesn't recognise
                        // (e.g. .task/.tflite) — fall back to direct HF download.
                        if (msg.find("no recognizable model files") != std::string::npos ||
                            msg.find("could not infer manifest") != std::string::npos) {
                            std::cout << "[AdminController] GenieX cannot handle repo '"
                                      << model << "' (" << msg
                                      << ") — falling back to HF direct download\n";
                            use_hf_fallback = true;
                        } else {
                            throw;  // network error or auth failure → propagate
                        }
                    }

                    if (use_hf_fallback) {
                        // HF installs to models_dir/<org>/<repo> (no double-prefix).
                        // Update the lock's target_path BEFORE starting the download
                        // so that a concurrent DELETE issued during the download is
                        // correctly blocked (findBlockingLock matches target_path).
                        fs::path hf_install = fs::path(models_dir) / model;
                        {
                            admin::LockJson updated = admin::buildUpdatedDownloadLockJson(
                                lock_path, models_dir, job->job_id, source, model,
                                runtime, precision, version, chipset, "downloading");
                            updated["target_path"] = hf_install.string();
                            if (!admin::writeJsonFile(lock_path, updated, lock_error)) {
                                LOG_WARN << "[AdminController] HF fallback: failed to update "
                                         << "lock target_path before download: " << lock_error;
                            }
                        }

                        HfDirectClient::pull(
                            model, models_dir,
                            [&job](int64_t done, int64_t total) {
                                job->bytes_downloaded.store(done);
                                job->total_bytes.store(total);
                            });

                        // HF direct path: scanModelBundles picks up hf_manifest.json
                        // and registers the model; resolve installed_id from registry.
                        job->status.store(FetchStatus::EXTRACTING);
                        {
                            admin::LockJson updated = admin::buildUpdatedDownloadLockJson(
                                lock_path, models_dir, job->job_id, source, model,
                                runtime, precision, version, chipset, "extracting");
                            updated["target_path"] = hf_install.string();
                            if (!admin::writeJsonFile(lock_path, updated, lock_error)) {
                                LOG_WARN << "[AdminController] HF fallback: failed to update "
                                         << "lock status to extracting: " << lock_error;
                            }
                        }
                        ModelConfigManager::getInstance().scanModelBundles();

                        // Resolve the registered id from the scan result.
                        // HF fallback IDs include the ModelFile quant key
                        // (usually "default"), so do not hand-build them.
                        std::string hf_id = findRegisteredModelIdByPath(hf_install, "");
                        if (hf_id.empty()) {
                            throw std::runtime_error(
                                "HF fallback: model installed at " + hf_install.string() +
                                " but not found in registry after scan");
                        }

                        job->setInstalled(hf_id, hf_install.string());
                        job->status.store(FetchStatus::DONE);
                        admin::removeDownloadLock(lock_path);
                        std::cout << "[AdminController] HF direct model installed: "
                                  << hf_id << " at " << hf_install.string() << "\n";
                        return;
                    }

                    // ── Hot-reload model registry ──────────────────────────────
                    job->status.store(FetchStatus::EXTRACTING);
                    admin::writeJsonFile(
                        lock_path,
                        admin::buildUpdatedDownloadLockJson(
                            lock_path, models_dir, job->job_id, source, model,
                            runtime, precision, version, chipset, "extracting"),
                        lock_error);
                    ModelConfigManager::getInstance().scanModelBundles();

                    // Detect cache hit: SDK skipped download (file already present)
                    if (job->bytes_downloaded.load() == 0) {
                        job->setCachedMessage("loaded from local cache, no download needed");
                    }

                    // Resolve the installed paths from the SDK.
                    GenieXClient::ModelPaths paths = GenieXClient::getPaths(model);
                    std::string rt = paths.plugin_id.empty() ? "qairt" : paths.plugin_id;
                    std::string mname = paths.model_name.empty() ? model : paths.model_name;
                    // Normalise mname: keep only the leaf after the last '/'
                    if (auto slash = mname.find_last_of('/'); slash != std::string::npos)
                        mname = mname.substr(slash + 1);

                    // Prefer the id that was actually registered by scanModelBundles so
                    // it includes the precision suffix (e.g. "...-q4_k_m-llamacpp").
                    // Fall back to the legacy mname+rt form only if nothing matched.
                    std::string registered_id;
                    {
                        std::string prec_lower = precision;
                        for (auto& c : prec_lower) c = static_cast<char>(std::tolower(c));
                        // normalizeRuntime strips underscores/hyphens: "llama_cpp" → "llamacpp"
                        std::string rt_norm = rt;
                        rt_norm.erase(std::remove(rt_norm.begin(), rt_norm.end(), '_'), rt_norm.end());
                        std::string candidate = mname + "-" + prec_lower + "-" + rt_norm;
                        if (ModelConfigManager::getInstance().getModelConfig(candidate)) {
                            registered_id = candidate;
                        } else {
                            // fallback: plain mname-rt (non-GGUF or precision not in ModelFile)
                            registered_id = mname + "-" + rt_norm;
                        }
                    }

                    // Confirm GenieX model was actually registered.
                    // findRegisteredModelIdByPath now matches against bundle_path
                    // (set by scanModelBundles for every bundle type), so this
                    // correctly resolves qairt/metadata.json bundles whose config_file
                    // lives in /tmp/configs rather than the install dir.
                    std::string verified_id = findRegisteredModelIdByPath(
                        paths.model_dir, registered_id, precision);

                    if (!verified_id.empty()) {
                        registered_id = verified_id;
                    }

                    // Strong check: registered_id must exist in the registry.
                    // If it does not, the model was installed on disk but not
                    // picked up by scanModelBundles — surface this as a real error.
                    if (!ModelConfigManager::getInstance().getModelConfig(registered_id)) {
                        throw std::runtime_error(
                            "GenieX model installed at " + paths.model_dir +
                            " but id '" + registered_id + "' not found in registry after scan");
                    }

                    job->setInstalled(registered_id, paths.model_dir);
                    job->status.store(FetchStatus::DONE);
                    admin::removeDownloadLock(lock_path);

                    std::cout << "[AdminController] GenieX model installed: "
                              << registered_id << " at " << paths.model_dir << "\n";
                    return;
                }

                // ── AI-Hub path (default): direct S3 download ──────────────────
                // ── Step 1: Resolve S3 URL ─────────────────────────────────────
                std::string url = AiHubClient::resolveUrl(
                    model, asset_rt, precision, version, chipset);
                int64_t zip_bytes = AiHubClient::contentLength(url);
                admin::requireAiHubFreeSpace(tmp_dir, models_dir, zip_bytes);

                // ── Step 2: Download ZIP ───────────────────────────────────────
                job->status.store(FetchStatus::DOWNLOADING);
                std::string lock_error;
                admin::writeJsonFile(
                    lock_path,
                    admin::buildUpdatedDownloadLockJson(
                        lock_path, models_dir, job->job_id, source, model,
                        runtime, precision, version, chipset, "downloading"),
                    lock_error);

                std::string zip_filename = model + "-" + asset_rt + "-" + precision;
                if (!chipset.empty()) zip_filename += "-" + chipset;
                zip_filename += "-v" + version + ".zip";

                std::string zip_path = tmp_dir + "/" + zip_filename + ".partial-" + job->job_id;
                fs::create_directories(tmp_dir);

                // Remove a stale per-job partial file if the same job id somehow
                // reuses the path after a process restart / retry.
                if (fs::exists(zip_path)) {
                    fs::remove(zip_path);
                }

                AiHubClient::download(
                    url, zip_path,
                    [&job](int64_t done, int64_t total) {
                        job->bytes_downloaded.store(done);
                        job->total_bytes.store(total);
                    });

                // ── Step 3: Extract ZIP ────────────────────────────────────────
                job->status.store(FetchStatus::EXTRACTING);
                admin::writeJsonFile(
                    lock_path,
                    admin::buildUpdatedDownloadLockJson(
                        lock_path, models_dir, job->job_id, source, model,
                        runtime, precision, version, chipset, "extracting"),
                    lock_error);

                // Destination directory name mirrors the ZIP filename without .zip
                // e.g. nomic_embed_text-qnn_dlc-float/
                std::string dest_name = model + "-" + asset_rt + "-" + precision;
                if (!chipset.empty()) dest_name += "-" + chipset;
                std::string dest_path = models_dir + "/" + dest_name;

                std::string extracted = AiHubClient::extractZip(zip_path, dest_path);

                // Clean up ZIP
                fs::remove(zip_path);

                // Validate ZIP contents — abort if no model manifest found
                {
                    static const std::vector<std::string> kManifestFiles = {
                        "metadata.json", "geniex.json", "model_config.json"
                    };
                    bool has_manifest = false;
                    for (const auto& f : kManifestFiles)
                        if (fs::exists(fs::path(extracted) / f)) { has_manifest = true; break; }
                    if (!has_manifest) {
                        std::error_code cleanup_ec;
                        fs::remove_all(extracted, cleanup_ec);
                        throw std::runtime_error(
                            "Extracted ZIP has no model manifest in " + extracted);
                    }
                }

                // AI Hub's metadata.json has no "model_type" field — backfill it
                // from "runtime" before the registry scans this bundle, so
                // predictive bundles (qnn/snpe/litert) aren't misclassified as
                // "generative" (ModelConfigManager's default).
                fs::path extracted_meta = fs::path(extracted) / "metadata.json";
                if (fs::exists(extracted_meta)) {
                    backfillModelType(extracted_meta);
                }

                // ── Step 4: Hot-reload model registry ─────────────────────────
                ModelConfigManager::getInstance().scanModelBundles();

                // Determine the installed model ID from metadata.json. Keep this
                // in sync with ModelConfigManager's metadata id convention:
                // "{model_id}-{runtime}-{precision}".
                std::string installed_id = model + "-" + asset_rt + "-" + precision;
                fs::path meta_path = fs::path(extracted) / "metadata.json";
                if (fs::exists(meta_path)) {
                    try {
                        std::ifstream f(meta_path);
                        json meta = json::parse(f);
                        std::string mid = meta.value("model_id", model);
                        std::string rt  = meta.value("runtime", asset_rt);
                        std::string prec = meta.value("precision", precision);
                        installed_id = mid + "-" + rt + "-" + prec;
                    } catch (...) {}
                }

                // Confirm model was actually registered (handles scan race).
                requireRegisteredModelId(installed_id);

                job->setInstalled(installed_id, extracted);
                job->status.store(FetchStatus::DONE);

                std::cout << "[AdminController] Model installed: " << installed_id
                          << " at " << extracted << "\n";
                admin::removeDownloadLock(lock_path);

            } catch (const std::exception& e) {
                job->setError(e.what());
                job->status.store(FetchStatus::FAILED);
                std::error_code cleanup_ec;
                if (source == "aihub") {
                    // Clean partial ZIP
                    std::string asset_rt = assetRuntime(runtime);
                    std::string zip_filename = model + "-" + asset_rt + "-" + precision;
                    if (!chipset.empty()) zip_filename += "-" + chipset;
                    zip_filename += "-v" + version + ".zip.partial-" + job->job_id;
                    fs::remove(fs::path(tmp_dir) / zip_filename, cleanup_ec);
                    if (cleanup_ec) {
                        LOG_WARN << "[AdminController] Failed to clean partial AI Hub ZIP "
                                 << (fs::path(tmp_dir) / zip_filename).string()
                                 << ": " << cleanup_ec.message();
                    }
                    // Clean partially extracted bundle directory if it exists and
                    // has no valid manifest (i.e. extraction was incomplete).
                    std::string dest_name = model + "-" + asset_rt + "-" + precision;
                    if (!chipset.empty()) dest_name += "-" + chipset;
                    fs::path dest_bundle = fs::path(models_dir) / dest_name;
                    if (fs::exists(dest_bundle, cleanup_ec)) {
                        bool has_manifest = fs::exists(dest_bundle / "metadata.json") ||
                                            fs::exists(dest_bundle / "geniex.json") ||
                                            fs::exists(dest_bundle / "model_config.json");
                        if (!has_manifest) {
                            fs::remove_all(dest_bundle, cleanup_ec);
                            if (cleanup_ec) {
                                LOG_WARN << "[AdminController] Failed to clean partial bundle "
                                         << dest_bundle.string() << ": " << cleanup_ec.message();
                            } else {
                                LOG_INFO << "[AdminController] Cleaned partial bundle: "
                                         << dest_bundle.string();
                            }
                        }
                    }
                } else if (source == "geniex") {
                    // GenieX SDK may leave empty bundle dirs and internal lock
                    // files (.lock, .inflight) on failure. If the bundle has no
                    // valid manifest, it is incomplete — remove it so the next
                    // retry starts clean.
                    auto cleanBundleIfEmpty = [&](const fs::path& bundle) {
                        if (!fs::exists(bundle, cleanup_ec)) return;
                        bool has_manifest = fs::exists(bundle / "geniex.json") ||
                                            fs::exists(bundle / "hf_manifest.json") ||
                                            fs::exists(bundle / "metadata.json");
                        if (!has_manifest) {
                            fs::remove_all(bundle, cleanup_ec);
                            if (!cleanup_ec) {
                                LOG_INFO << "[AdminController] Cleaned incomplete GenieX bundle: "
                                         << bundle.string();
                            } else {
                                LOG_WARN << "[AdminController] Failed to clean incomplete GenieX bundle "
                                         << bundle.string() << ": " << cleanup_ec.message();
                            }
                        }
                    };
                    // Clean both the direct path and the double-prefix path that
                    // GenieX SDK sometimes uses (GENIEX_DATADIR/models/org/repo).
                    cleanBundleIfEmpty(fs::path(models_dir) / model);
                    cleanBundleIfEmpty(fs::path(models_dir) / "models" / model);
                    // Also remove empty parent org directory left behind
                    {
                        fs::path org_dir = (fs::path(models_dir) / "models" / model).parent_path();
                        if (fs::exists(org_dir, cleanup_ec)) {
                            bool org_empty = true;
                            for (const auto& e : fs::directory_iterator(org_dir, cleanup_ec))
                                { org_empty = false; break; }
                            if (org_empty) fs::remove(org_dir, cleanup_ec);
                        }
                    }
                }
                LOG_ERROR << "[AdminController] Model fetch failed"
                          << " job_id=" << job->job_id
                          << " source=" << source
                          << " model=" << model
                          << " runtime=" << runtime
                          << " precision=" << precision
                          << " version=" << version
                          << " chipset=" << chipset
                          << " error=" << e.what();
                admin::removeDownloadLock(lock_path);
            }
        });
        worker.detach();
    } catch (const std::exception& e) {
        job->setError(e.what());
        job->status.store(FetchStatus::FAILED);
        admin::removeDownloadLock(lock_path);
        callback(errorResp(
            std::string("Failed to start model download worker: ") + e.what(),
            k500InternalServerError));
        return;
    }

    // Return 202 Accepted immediately
    callback(jsonResp({
        {"job_id", job_id},
        {"status", "pending"},
        {"model",  model},
        {"runtime", runtime},
        {"precision", precision},
        {"version", version},
        {"source", source}
    }, k202Accepted));
}

// ─────────────────────────────────────────────────────────────────────────────
// GET /admin/models/fetch/{job_id}
// ─────────────────────────────────────────────────────────────────────────────
void AdminController::getFetchJob(const HttpRequestPtr& req,
                                   std::function<void(const HttpResponsePtr&)>&& callback,
                                   const std::string& job_id) {
    if (!checkAuth(req, callback)) return;

    auto job = ModelFetchJobRegistry::getInstance().get(job_id);
    if (!job) {
        callback(errorResp("Job not found: " + job_id, k404NotFound));
        return;
    }

    callback(jsonResp(job->toJson()));
}

// ─────────────────────────────────────────────────────────────────────────────
// POST /admin/models/reload
// ─────────────────────────────────────────────────────────────────────────────
void AdminController::reloadModels(const HttpRequestPtr& req,
                                    std::function<void(const HttpResponsePtr&)>&& callback) {
    if (!checkAuth(req, callback)) return;

    ModelConfigManager::getInstance().scanModelBundles();

    auto models = ModelConfigManager::getInstance().getAvailableModels();
    json arr = json::array();
    for (const auto& m : models) {
        arr.push_back(modelSummaryJson(m));
    }
    callback(jsonResp({{"reloaded", true}, {"models", arr}}));
}

// GET /admin/models
// ─────────────────────────────────────────────────────────────────────────────
void AdminController::listModels(const HttpRequestPtr& req,
                                  std::function<void(const HttpResponsePtr&)>&& callback) {
    if (!checkAuth(req, callback)) return;

    auto models = ModelConfigManager::getInstance().getAvailableModels();

    json arr = json::array();
    for (const auto& m : models) {
        arr.push_back(modelSummaryJson(m));
    }

    callback(jsonResp(arr));
}

// ─────────────────────────────────────────────────────────────────────────────
// GET /admin/models/{model_id}
// ─────────────────────────────────────────────────────────────────────────────
void AdminController::getModel(const HttpRequestPtr& req,
                                std::function<void(const HttpResponsePtr&)>&& callback,
                                const std::string& model_id) {
    if (!checkAuth(req, callback)) return;

    const ModelConfig* m = ModelConfigManager::getInstance().getModelConfig(model_id);
    if (!m) {
        callback(errorResp("Model not found: " + model_id, k404NotFound));
        return;
    }

    callback(jsonResp(modelSummaryJson(*m, true)));
}

// ─────────────────────────────────────────────────────────────────────────────
// DELETE /admin/models/{model_id}
// ─────────────────────────────────────────────────────────────────────────────
void AdminController::deleteModel(const HttpRequestPtr& req,
                                   std::function<void(const HttpResponsePtr&)>&& callback,
                                   const std::string& model_id) {
    if (!checkAuth(req, callback)) return;

    const ModelConfig* m = ModelConfigManager::getInstance().getModelConfig(model_id);
    if (!m) {
        callback(jsonResp({{"deleted", model_id},
                           {"message", "model not found, nothing to delete"}},
                          k200OK));
        return;
    }

    // Derive the bundle directory from the config_file path
    // config_file is in /tmp/configs/<bundle_name>/... — we need the original
    // bundle in the models directory.
    // Strategy: scan models_dir for a directory whose metadata.json yields this model_id
    std::string models_dir = modelsDir();
    std::string bundle_to_delete;

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(models_dir, ec)) {
        if (!entry.is_directory()) continue;
        fs::path meta = entry.path() / "metadata.json";
        if (!fs::exists(meta)) continue;
        try {
            std::ifstream f(meta);
            json metadata = json::parse(f);
            std::string mid = metadata.value("model_id", "");
            std::string rt  = metadata.value("runtime", "genie");
            std::string prec = metadata.value("precision", "float");
            if ((mid + "-" + rt + "-" + prec) == model_id) {
                bundle_to_delete = entry.path().string();
                break;
            }
        } catch (...) {}
    }

    if (bundle_to_delete.empty()) {
        // metadata.json not found — fallback for GGUF/bare-file bundles.
        // config_file points directly to the .gguf file; its parent directory
        // is the bundle directory. Verify it lives somewhere under models_dir.
        fs::path cfg(m->config_file);
        fs::path candidate = cfg.parent_path();
        fs::path models_path(models_dir);
        // Ensure candidate is under models_dir and is not models_dir itself
        if (isPathInsideDirectory(candidate, models_path)) {
            bundle_to_delete = candidate.lexically_normal().string();
        }
    }

    if (bundle_to_delete.empty()) {
        // Still not found — rescan to sync in-memory registry and report
        ModelConfigManager::getInstance().scanModelBundles();
        callback(jsonResp({{"deleted", model_id},
                           {"message", "model not found on disk, nothing to delete"}},
                          k200OK));
        return;
    }

    // Safety check: bundle_to_delete must be under models_dir and not models_dir itself
    {
        fs::path bundle_path = fs::path(bundle_to_delete).lexically_normal();
        fs::path models_path = fs::path(models_dir).lexically_normal();
        if (!isPathInsideDirectory(bundle_path, models_path)) {
            LOG_ERROR << "[AdminController] Refusing to delete unsafe path: "
                      << bundle_path.string() << " (models_dir="
                      << models_path.string() << ")";
            callback(errorResp("Unsafe delete path rejected: " + bundle_path.string(),
                               k500InternalServerError));
            return;
        }
        LOG_INFO << "[AdminController] Will delete bundle: " << bundle_path.string();
    }

    fs::path blocking_lock_path;
    admin::LockJson blocking_lock;
    admin::pruneStaleDownloadLocks(models_dir);

    if (admin::findBlockingLock(
            models_dir, bundle_to_delete, true, blocking_lock_path, blocking_lock)) {
        callback(jsonResp({
            {"error", "Model is currently locked by an active download or use lease."},
            {"details", {
                {"lock_file", blocking_lock_path.string()},
                {"lock", blocking_lock},
            }},
        }, k409Conflict));
        return;
    }

    // For GenieX GGUF bundles (geniex.json present), multiple precisions share the
    // same bundle directory. Only remove the specific .gguf file and its ModelFile
    // entry; delete the whole directory only when no downloaded precisions remain.
    fs::path geniex_json_path = fs::path(bundle_to_delete) / "geniex.json";
    std::error_code rm_ec;
    bool geniex_bundle = fs::exists(geniex_json_path);

    if (geniex_bundle) {
        // Remove only the specific .gguf file named in config_file
        fs::path gguf_file(m->config_file);
        if (fs::exists(gguf_file)) {
            fs::remove(gguf_file, rm_ec);
            if (rm_ec) {
                callback(errorResp(
                    "Failed to remove GGUF file: " + rm_ec.message(),
                    k500InternalServerError));
                return;
            }
            LOG_INFO << "[AdminController] Removed GGUF file: " << gguf_file;
        }

        // Update geniex.json: remove this quant key from ModelFile
        try {
            std::ifstream fi(geniex_json_path);
            json manifest = json::parse(fi);
            fi.close();

            auto& model_file = manifest["ModelFile"];
            if (model_file.is_object()) {
                // Find and remove the key whose Name matches the deleted file
                std::string fname = gguf_file.filename().string();
                for (auto it = model_file.begin(); it != model_file.end(); ++it) {
                    if (it.value().value("Name", "") == fname) {
                        // Mark as not downloaded so GenieX SDK re-downloads next time
                        // instead of treating the missing file as a cache hit.
                        it.value()["Downloaded"] = false;
                        break;
                    }
                }
            }

            std::ofstream fo(geniex_json_path);
            fo << manifest.dump(2);
        } catch (const std::exception& e) {
            LOG_WARN << "[AdminController] Could not update geniex.json: " << e.what();
        }

        // Check if any downloaded precisions remain (Downloaded=true AND file exists).
        bool any_remaining = false;
        try {
            std::ifstream fi(geniex_json_path);
            json manifest = json::parse(fi);
            auto& mf = manifest["ModelFile"];
            if (mf.is_object()) {
                for (const auto& [k, v] : mf.items()) {
                    if (!v.value("Downloaded", false)) continue;
                    std::string fname = v.value("Name", "");
                    if (fname.empty()) continue;
                    if (fs::exists(fs::path(bundle_to_delete) / fname)) {
                        any_remaining = true;
                        break;
                    }
                }
            }
        } catch (...) {}

        if (!any_remaining) {
            fs::remove_all(bundle_to_delete, rm_ec);
            LOG_INFO << "[AdminController] Removed empty GenieX bundle: " << bundle_to_delete;
        }
    } else {
        // aihub / bare bundle — remove the whole directory as before
        fs::remove_all(bundle_to_delete, rm_ec);
    }

    if (rm_ec) {
        callback(errorResp(
            "Failed to remove model directory: " + rm_ec.message(),
            k500InternalServerError));
        return;
    }

    // Hot-reload model registry
    ModelConfigManager::getInstance().scanModelBundles();

    std::cout << "[AdminController] Deleted model: " << model_id
              << " (" << bundle_to_delete << ")\n";

    callback(jsonResp({
        {"deleted",    model_id},
        {"directory",  bundle_to_delete},
    }));
}

// ─────────────────────────────────────────────────────────────────────────────
// GET /admin/models/versions
// ─────────────────────────────────────────────────────────────────────────────
void AdminController::listVersions(const HttpRequestPtr& req,
                                    std::function<void(const HttpResponsePtr&)>&& callback) {
    if (!checkAuth(req, callback)) return;

    try {
        auto versions = AiHubClient::fetchVersions();
        json arr = json::array();
        for (const auto& v : versions) arr.push_back(v);
        callback(jsonResp({{"versions", arr}}));
    } catch (const std::exception& e) {
        callback(errorResp(
            std::string("Failed to fetch versions: ") + e.what(),
            k502BadGateway));
    }
}
