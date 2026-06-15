// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <shared_mutex>

// ─────────────────────────────────────────────────────────────────────────────
// SystemResourceManager — P9: Memory availability + LRU worker eviction
//
// Checks /proc/meminfo to determine if sufficient memory is available before
// loading a new model. If memory is insufficient, evicts idle inference workers
// using an LRU (Least Recently Used) policy.
//
// Used by InferenceWorkerManager before calling ensureWorkerRunning().
// ─────────────────────────────────────────────────────────────────────────────

struct ProcessInfo {
    std::string process_id;
    std::string model_name;
    long memory_mb = 0;
    bool is_active = false;
    long last_used_timestamp = 0;  // Unix timestamp
};

class SystemResourceManager {
public:
    static SystemResourceManager& getInstance();

    // ── Memory checks ──────────────────────────────────────────────────────────

    /**
     * Get available system memory in MB (from /proc/meminfo MemAvailable).
     */
    long getAvailableMemoryMb() const;

    /**
     * Check if sufficient memory is available for a model requiring memory_mb.
     * Returns true if available memory > memory_mb + MEMORY_HEADROOM_MB.
     */
    bool hasEnoughMemory(long memory_mb) const;

    /**
     * Check memory availability and determine which idle processes to evict.
     *
     * @param required_mb  Memory required for the new model
     * @return             List of process_ids to evict (LRU order), or empty if enough memory
     */
    std::vector<std::string> getProcessesToEvict(long required_mb) const;

    // ── Process registry ───────────────────────────────────────────────────────

    void registerProcess(const std::string& process_id, const std::string& model_name,
                         long memory_mb, bool is_active);
    void unregisterProcess(const std::string& process_id);
    void markProcessActive(const std::string& process_id);
    void markProcessIdle(const std::string& process_id);

    bool isProcessRegistered(const std::string& process_id) const;

private:
    SystemResourceManager() = default;
    SystemResourceManager(const SystemResourceManager&) = delete;
    SystemResourceManager& operator=(const SystemResourceManager&) = delete;

    // Minimum free memory headroom to keep after loading a model (MB)
    static constexpr long MEMORY_HEADROOM_MB = 1024;

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, ProcessInfo> processes_;
};
