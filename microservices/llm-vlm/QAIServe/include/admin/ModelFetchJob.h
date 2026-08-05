// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// ModelFetchJob — Async model download job state
//
// Tracks the progress of a background model download from AI Hub.
// All progress fields are atomic so they can be read from any thread
// without holding a lock.
//
// Lifecycle:
//   PENDING → DOWNLOADING → EXTRACTING → DONE
//                                      ↘ FAILED
// ─────────────────────────────────────────────────────────────────────────────

#include <string>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <shared_mutex>
#include <cstdint>
#include <cstddef>
#include <nlohmann/json.hpp>

enum class FetchStatus {
    PENDING,
    DOWNLOADING,
    EXTRACTING,
    DONE,
    FAILED
};

// ─────────────────────────────────────────────────────────────────────────────
// ModelFetchJob — single job record
// ─────────────────────────────────────────────────────────────────────────────
struct ModelFetchJob {
    // Request parameters (immutable after creation)
    std::string job_id;
    std::string model;
    std::string runtime;
    std::string precision;
    std::string version;
    std::string chipset;
    std::string source;   // download source: "aihub" (default) or "geniex"

    // Progress (written by background thread, read by HTTP handler)
    std::atomic<FetchStatus> status{FetchStatus::PENDING};
    std::atomic<int64_t>     bytes_downloaded{0};
    std::atomic<int64_t>     total_bytes{0};

    // Set on completion or failure. Protected because background download
    // threads update these while GET /admin/models/fetch/{job_id} reads them.
    mutable std::mutex state_mutex;
    std::string error;
    std::string installed_id;   // e.g. "nomic_embed_text-qnn_dlc" on success
    std::string installed_path; // absolute path to extracted directory
    bool        cached{false};  // true when file already existed locally (no download)
    std::string message;        // human-readable note (e.g. "loaded from local cache")

    // Convenience
    double progress() const {
        int64_t total = total_bytes.load();
        if (total <= 0) return 0.0;
        return static_cast<double>(bytes_downloaded.load()) / static_cast<double>(total);
    }

    static std::string statusString(FetchStatus s) {
        switch (s) {
            case FetchStatus::PENDING:     return "pending";
            case FetchStatus::DOWNLOADING: return "downloading";
            case FetchStatus::EXTRACTING:  return "extracting";
            case FetchStatus::DONE:        return "done";
            case FetchStatus::FAILED:      return "failed";
        }
        return "unknown";
    }

    void setError(const std::string& value) {
        std::lock_guard<std::mutex> lock(state_mutex);
        error = value;
    }

    void setInstalled(const std::string& id, const std::string& path) {
        std::lock_guard<std::mutex> lock(state_mutex);
        installed_id = id;
        installed_path = path;
    }

    void setCachedMessage(const std::string& value) {
        std::lock_guard<std::mutex> lock(state_mutex);
        cached = true;
        message = value;
    }

    nlohmann::json toJson() const {
        FetchStatus s = status.load();
        nlohmann::json j = {
            {"job_id",           job_id},
            {"model",            model},
            {"runtime",          runtime},
            {"precision",        precision},
            {"version",          version},
            {"source",           source.empty() ? "aihub" : source},
            {"status",           statusString(s)},
            {"bytes_downloaded", bytes_downloaded.load()},
            {"total_bytes",      total_bytes.load()},
            {"progress",         progress()},
        };
        if (!chipset.empty()) j["chipset"] = chipset;

        std::lock_guard<std::mutex> lock(state_mutex);
        if (!error.empty())          j["error"]          = error;
        if (!installed_id.empty())   j["installed_id"]   = installed_id;
        if (!installed_path.empty()) j["installed_path"] = installed_path;
        if (cached)                  j["cached"]         = true;
        if (!message.empty())        j["message"]        = message;
        return j;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// ModelFetchJobRegistry — thread-safe singleton job store
// ─────────────────────────────────────────────────────────────────────────────
class ModelFetchJobRegistry {
public:
    static ModelFetchJobRegistry& getInstance();

    /**
     * Create a new job and register it.
     * Returns a shared_ptr to the job (background thread holds a reference).
     */
    std::shared_ptr<ModelFetchJob> create(
        const std::string& model,
        const std::string& runtime,
        const std::string& precision,
        const std::string& version,
        const std::string& chipset,
        const std::string& source = "aihub");

    /**
     * Create a new job only if the current number of active jobs is below
     * max_active. active_count is set to the count observed under the registry
     * lock before creation. Returns nullptr when the limit has been reached.
     */
    std::shared_ptr<ModelFetchJob> createIfBelowLimit(
        const std::string& model,
        const std::string& runtime,
        const std::string& precision,
        const std::string& version,
        const std::string& chipset,
        const std::string& source,
        size_t max_active,
        size_t& active_count);

    /**
     * Look up a job by ID. Returns nullptr if not found.
     */
    std::shared_ptr<ModelFetchJob> get(const std::string& job_id) const;

    /**
     * Remove completed/failed jobs older than max_age_seconds.
     * Called periodically to prevent unbounded memory growth.
     */
    void pruneOldJobs(int max_age_seconds = 3600);

private:
    ModelFetchJobRegistry() = default;
    ModelFetchJobRegistry(const ModelFetchJobRegistry&) = delete;
    ModelFetchJobRegistry& operator=(const ModelFetchJobRegistry&) = delete;

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<ModelFetchJob>> jobs_;

    static std::string generateJobId();
};
