// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// AdminController — Admin-only model management REST API
//
// All endpoints require the X-Admin-Token header to match the value of the
// QAISERVE_ADMIN_TOKEN environment variable. Requests with a missing or
// incorrect token receive 403 Forbidden.
//
// Routes:
//   POST   /admin/models/fetch              — Start async AI Hub download
//   GET    /admin/models/fetch/{job_id}     — Poll download progress
//   POST   /admin/models/reload             — Rescan models directory (hot reload)
//   GET    /admin/models                    — List installed models
//   GET    /admin/models/{model_id}         — Get single model metadata
//   DELETE /admin/models/{model_id}         — Remove a model
//   GET    /admin/models/versions           — List available AI Hub versions
// ─────────────────────────────────────────────────────────────────────────────

#include <drogon/HttpController.h>
#include <string>

using namespace drogon;

class AdminController : public drogon::HttpController<AdminController> {
public:
    METHOD_LIST_BEGIN
        // POST /admin/models/fetch — start async download from AI Hub
        ADD_METHOD_TO(AdminController::fetchModel,
                      "/admin/models/fetch", Post, Options);

        // GET /admin/models/fetch/{job_id} — poll download progress
        ADD_METHOD_TO(AdminController::getFetchJob,
                      "/admin/models/fetch/{1}", Get, Options);

        // POST /admin/models/reload — rescan models directory (hot reload)
        // Useful after manually adb-pushing model files to the models directory.
        ADD_METHOD_TO(AdminController::reloadModels,
                      "/admin/models/reload", Post, Options);

        // GET /admin/models/versions — list available AI Hub versions
        // NOTE: registered before /admin/models/{model_id} to avoid
        // "versions" being captured as a model_id path parameter.
        ADD_METHOD_TO(AdminController::listVersions,
                      "/admin/models/versions", Get, Options);

        // GET /admin/models — list all installed models
        ADD_METHOD_TO(AdminController::listModels,
                      "/admin/models", Get, Options);

        // GET /admin/models/{model_id} — get single model metadata
        ADD_METHOD_TO(AdminController::getModel,
                      "/admin/models/{1}", Get, Options);

        // DELETE /admin/models/{model_id} — remove a model
        ADD_METHOD_TO(AdminController::deleteModel,
                      "/admin/models/{1}", Delete, Options);
    METHOD_LIST_END

    /**
     * POST /admin/models/fetch
     *
     * Body (JSON):
     *   {
     *     "model":     "nomic_embed_text",   // required
     *     "runtime":   "qnn_dlc",            // required for source=aihub
     *     "precision": "float",              // optional, default "float"
     *     "version":   "0.45.0",             // optional (aihub), default latest
     *     "chipset":   "qcs8550",            // optional
     *     "source":    "aihub"               // optional: "aihub" (default) | "geniex"
     *   }
     *
     * source=aihub: direct S3 download of a qai-hub-models asset (AiHubClient).
     * source=geniex: delegate to the GenieX SDK, which resolves the hub
     *   (HuggingFace / AI Hub / …) from the model name; "runtime"/"version"
     *   are ignored. Example model: "ai-hub-models/Qwen3-4B-Instruct-2507".
     *
     * Returns 202 Accepted:
     *   { "job_id": "...", "status": "pending" }
     *
     * Starts a background thread that:
     *   1. Resolves the S3 URL (HEAD check) — aihub; or calls geniex_model_pull
     *   2. Downloads the ZIP with progress tracking (aihub)
     *   3. Extracts to the models directory (aihub)
     *   4. Calls ModelConfigManager::scanModelBundles() to hot-reload
     */
    void fetchModel(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback);

    /**
     * GET /admin/models/fetch/{job_id}
     *
     * Returns the current state of a fetch job:
     *   {
     *     "job_id":           "...",
     *     "status":           "downloading|extracting|done|failed",
     *     "progress":         0.42,
     *     "bytes_downloaded": 44040192,
     *     "total_bytes":      104857600,
     *     "installed_id":     "nomic_embed_text-qnn_dlc",  // on success
     *     "error":            "..."                         // on failure
     *   }
     */
    void getFetchJob(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback,
                     const std::string& job_id);

    /**
     * GET /admin/models
     *
     * Returns an array of all installed models (from ModelConfigManager):
     *   [
     *     {
     *       "id":           "nomic_embed_text-qnn_dlc",
     *       "model_name":   "Nomic-Embed-Text",
     *       "runtime":      "qnn_dlc",
     *       "model_type":   "predictive",
     *       "supports_vision": false
     *     },
     *     ...
     *   ]
     */
    void listModels(const HttpRequestPtr& req,
                    std::function<void(const HttpResponsePtr&)>&& callback);

    /**
     * GET /admin/models/{model_id}
     *
     * Returns metadata for a single installed model.
     * model_id format: "{model_id}-{runtime}" (e.g. "nomic_embed_text-qnn_dlc")
     */
    void getModel(const HttpRequestPtr& req,
                  std::function<void(const HttpResponsePtr&)>&& callback,
                  const std::string& model_id);

    /**
     * DELETE /admin/models/{model_id}
     *
     * Removes the model directory from disk and reloads the model registry.
     * model_id format: "{model_id}-{runtime}" (e.g. "nomic_embed_text-qnn_dlc")
     *
     * Returns 200 on success, 404 if model not found.
     */
    void deleteModel(const HttpRequestPtr& req,
                     std::function<void(const HttpResponsePtr&)>&& callback,
                     const std::string& model_id);

    /**
     * POST /admin/models/reload
     *
     * Rescans the models directory and hot-reloads the model registry.
     * Use this after manually pushing model files to the models directory
     * without going through the Admin fetch API.
     *
     * Returns 200 with the updated model list:
     *   { "reloaded": true, "models": [...] }
     */
    void reloadModels(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback);

    /**
     * GET /admin/models/versions
     *
     * Returns the list of available AI Hub Models versions from PyPI,
     * sorted newest-first:
     *   { "versions": ["0.45.0", "0.44.2", "0.44.1", ...] }
     */
    void listVersions(const HttpRequestPtr& req,
                      std::function<void(const HttpResponsePtr&)>&& callback);

private:
    /**
     * Check the X-Admin-Token header against QAISERVE_ADMIN_TOKEN env var.
     * Returns true if authorized, false otherwise.
     * Sends a 403 response and returns false if unauthorized.
     */
    bool checkAuth(const HttpRequestPtr& req,
                   std::function<void(const HttpResponsePtr&)>& callback);
};
