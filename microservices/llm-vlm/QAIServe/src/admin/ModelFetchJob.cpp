// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "admin/ModelFetchJob.h"

#include <chrono>
#include <mutex>
#include <random>
#include <sstream>
#include <iomanip>

// ─────────────────────────────────────────────────────────────────────────────
// ModelFetchJobRegistry — Singleton
// ─────────────────────────────────────────────────────────────────────────────

ModelFetchJobRegistry& ModelFetchJobRegistry::getInstance() {
    static ModelFetchJobRegistry instance;
    return instance;
}

// ─────────────────────────────────────────────────────────────────────────────
// generateJobId — produce a short random hex ID
// ─────────────────────────────────────────────────────────────────────────────
std::string ModelFetchJobRegistry::generateJobId() {
    // Use current time + random bytes for uniqueness
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::mt19937_64 rng(static_cast<uint64_t>(now));
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t val = dist(rng);

    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << val;
    return oss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
// create — allocate and register a new job
// ─────────────────────────────────────────────────────────────────────────────
std::shared_ptr<ModelFetchJob> ModelFetchJobRegistry::create(
    const std::string& model,
    const std::string& runtime,
    const std::string& precision,
    const std::string& version,
    const std::string& chipset,
    const std::string& source)
{
    auto job = std::make_shared<ModelFetchJob>();
    job->job_id    = generateJobId();
    job->model     = model;
    job->runtime   = runtime;
    job->precision = precision;
    job->version   = version;
    job->chipset   = chipset;
    job->source    = source;

    std::unique_lock lock(mutex_);
    jobs_[job->job_id] = job;
    return job;
}

// ─────────────────────────────────────────────────────────────────────────────
// get — look up a job by ID
// ─────────────────────────────────────────────────────────────────────────────
std::shared_ptr<ModelFetchJob> ModelFetchJobRegistry::get(const std::string& job_id) const {
    std::shared_lock lock(mutex_);
    auto it = jobs_.find(job_id);
    return (it != jobs_.end()) ? it->second : nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// pruneOldJobs — remove completed/failed jobs to prevent memory growth
//
// Since ModelFetchJob doesn't store a timestamp, we prune all jobs that are
// in a terminal state (DONE or FAILED) when the registry grows large.
// A more precise implementation would store a completion timestamp.
// ─────────────────────────────────────────────────────────────────────────────
void ModelFetchJobRegistry::pruneOldJobs(int /*max_age_seconds*/) {
    std::unique_lock lock(mutex_);
    // Keep at most 100 jobs; remove terminal ones when over limit
    if (jobs_.size() <= 100) return;

    for (auto it = jobs_.begin(); it != jobs_.end(); ) {
        FetchStatus s = it->second->status.load();
        if (s == FetchStatus::DONE || s == FetchStatus::FAILED) {
            it = jobs_.erase(it);
        } else {
            ++it;
        }
        if (jobs_.size() <= 50) break;
    }
}
