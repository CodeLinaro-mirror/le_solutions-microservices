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
#include "admin/GenieXClient.h"
#include "admin/ModelFetchJob.h"
#include "qai_forge/managers/ModelConfigManager.h"

#include <drogon/HttpResponse.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <thread>
#include <cstdlib>
#include <iostream>
#include <set>

namespace fs = std::filesystem;
// Note: ModelConfigManager.h already declares `using json = nlohmann::ordered_json`
// at file scope; do not redeclare here to avoid a conflicting-declaration error.

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

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

    std::string model     = body.value("model", "");
    std::string runtime   = body.value("runtime", "");
    std::string precision = body.value("precision", "float");
    std::string version   = body.value("version", "");
    std::string chipset   = body.value("chipset", "");
    std::string source    = body.value("source", "aihub");

    if (model.empty()) {
        callback(errorResp("'model' is required.", k400BadRequest));
        return;
    }
    if (source != "aihub" && source != "geniex") {
        callback(errorResp("'source' must be 'aihub' or 'geniex'.", k400BadRequest));
        return;
    }
    // 'runtime' is required only for the aihub S3 path (it is part of the asset
    // URL). GenieX resolves the runtime itself from the model repo.
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
    // wastes a full download just to fail. dest_name mirrors the naming used
    // in the background thread below.
    {
        std::string asset_rt = assetRuntime(runtime);
        std::string dest_name = model + "-" + asset_rt + "-" + precision;
        if (!chipset.empty()) dest_name += "-" + chipset;
        std::string dest_path = modelsDir() + "/" + dest_name;
        if (fs::exists(dest_path)) {
            callback(errorResp(
                "Model already installed at " + dest_path
                    + " — delete it first (DELETE /admin/models/{model_id}) to re-fetch.",
                k409Conflict));
            return;
        }
    }

    // Create job
    auto& registry = ModelFetchJobRegistry::getInstance();
    registry.pruneOldJobs();
    auto job = registry.create(model, runtime, precision, version, chipset, source);

    // Capture job_id for the response (job ptr is moved into the thread)
    std::string job_id = job->job_id;

    // Launch background download thread
    std::thread([job, model, runtime, precision, version, chipset, source]() {
        const std::string models_dir = modelsDir();
        const std::string tmp_dir    = tmpDownloadDir();
        const std::string asset_rt   = assetRuntime(runtime);

        try {
            if (source == "geniex") {
                // ── GenieX path: delegate to the native SDK ────────────────
                // The SDK resolves the hub (HuggingFace / AI Hub / …), downloads
                // (with resume), and writes the bundle + geniex.json manifest
                // under its data dir. We point that data dir at the scanned
                // models directory so the hot-reload picks it up.
                job->status.store(FetchStatus::DOWNLOADING);

                GenieXClient::pull(
                    model, precision,
                    GenieXClient::Hub::Auto,   // auto-select hub from repo name
                    chipset, models_dir,
                    [&job](int64_t done, int64_t total) {
                        job->bytes_downloaded.store(done);
                        job->total_bytes.store(total);
                    });

                // ── Hot-reload model registry ──────────────────────────────
                job->status.store(FetchStatus::EXTRACTING);
                ModelConfigManager::getInstance().scanModelBundles();

                // Resolve the installed paths from the SDK.
                GenieXClient::ModelPaths paths = GenieXClient::getPaths(model);
                std::string rt = paths.plugin_id.empty() ? "qairt" : paths.plugin_id;
                std::string mname = paths.model_name.empty() ? model : paths.model_name;

                job->installed_id   = mname + "-" + rt;
                job->installed_path = paths.model_dir;
                job->status.store(FetchStatus::DONE);

                std::cout << "[AdminController] GenieX model installed: "
                          << job->installed_id << " at " << paths.model_dir << "\n";
                return;
            }

            // ── AI-Hub path (default): direct S3 download ──────────────────
            // ── Step 1: Resolve S3 URL ─────────────────────────────────────
            std::string url = AiHubClient::resolveUrl(
                model, asset_rt, precision, version, chipset);

            // ── Step 2: Download ZIP ───────────────────────────────────────
            job->status.store(FetchStatus::DOWNLOADING);

            std::string zip_filename = model + "-" + asset_rt + "-" + precision;
            if (!chipset.empty()) zip_filename += "-" + chipset;
            zip_filename += "-v" + version + ".zip";

            std::string zip_path = tmp_dir + "/" + zip_filename;
            fs::create_directories(tmp_dir);

            // Remove stale partial download if it exists from a previous attempt
            // that was not a resume (different version/params)
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

            // Destination directory name mirrors the ZIP filename without .zip
            // e.g. nomic_embed_text-qnn_dlc-float/
            std::string dest_name = model + "-" + asset_rt + "-" + precision;
            if (!chipset.empty()) dest_name += "-" + chipset;
            std::string dest_path = models_dir + "/" + dest_name;

            std::string extracted = AiHubClient::extractZip(zip_path, dest_path);

            // Clean up ZIP
            fs::remove(zip_path);

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

            // Determine the installed model ID from metadata.json
            std::string installed_id = model + "-" + runtime;
            fs::path meta_path = fs::path(extracted) / "metadata.json";
            if (fs::exists(meta_path)) {
                try {
                    std::ifstream f(meta_path);
                    json meta = json::parse(f);
                    std::string mid = meta.value("model_id", model);
                    std::string rt  = meta.value("runtime", runtime);
                    installed_id = mid + "-" + rt;
                } catch (...) {}
            }

            job->installed_id   = installed_id;
            job->installed_path = extracted;
            job->status.store(FetchStatus::DONE);

            std::cout << "[AdminController] Model installed: " << installed_id
                      << " at " << extracted << "\n";

        } catch (const std::exception& e) {
            job->error = e.what();
            job->status.store(FetchStatus::FAILED);
            std::cerr << "[AdminController] Fetch failed for "
                      << model << " (" << source << "): " << e.what() << "\n";
        }
    }).detach();

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
        arr.push_back({
            {"id",              m.id},
            {"model_name",      m.display_name},
            {"runtime",         m.runtime},
            {"model_type",      m.model_type},
            {"supports_vision", m.supports_vision},
            {"context_size",    m.context_size},
            {"memory_mb",       m.memory_requirement_mb}
        });
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
        arr.push_back({
            {"id",              m.id},
            {"model_name",      m.display_name},
            {"runtime",         m.runtime},
            {"model_type",      m.model_type},
            {"supports_vision", m.supports_vision},
            {"supports_thinking", m.supports_thinking},
            {"context_size",    m.context_size},
            {"memory_mb",       m.memory_requirement_mb},
        });
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

    callback(jsonResp({
        {"id",                m->id},
        {"model_name",        m->display_name},
        {"runtime",           m->runtime},
        {"model_type",        m->model_type},
        {"supports_vision",   m->supports_vision},
        {"supports_thinking", m->supports_thinking},
        {"supports_streaming",m->supports_streaming},
        {"context_size",      m->context_size},
        {"memory_mb",         m->memory_requirement_mb},
        {"config_file",       m->config_file},
    }));
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
        std::string cand_str = candidate.string();
        std::string mdir_str = models_path.string();
        if (!cand_str.empty() &&
            cand_str.size() > mdir_str.size() &&
            cand_str.substr(0, mdir_str.size()) == mdir_str) {
            bundle_to_delete = cand_str;
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
        std::string b = fs::path(bundle_to_delete).lexically_normal().string();
        std::string m = fs::path(models_dir).lexically_normal().string();
        if (b == m || b.size() <= m.size() ||
            b.substr(0, m.size()) != m ||
            b[m.size()] != '/') {
            LOG_ERROR << "[AdminController] Refusing to delete unsafe path: " << b
                      << " (models_dir=" << m << ")";
            callback(errorResp("Unsafe delete path rejected: " + b,
                               k500InternalServerError));
            return;
        }
        LOG_INFO << "[AdminController] Will delete bundle: " << b;
    }

    // Remove the bundle directory
    std::error_code rm_ec;
    fs::remove_all(bundle_to_delete, rm_ec);
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
