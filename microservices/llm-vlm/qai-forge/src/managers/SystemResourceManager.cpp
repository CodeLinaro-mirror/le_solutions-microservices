// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/managers/SystemResourceManager.h"
#include "qai_forge/utils/Logger.h"
#include <fstream>
#include <algorithm>
#include <chrono>
#include <mutex>

SystemResourceManager& SystemResourceManager::getInstance() {
    static SystemResourceManager instance;
    return instance;
}

long SystemResourceManager::getAvailableMemoryMb() const {
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo.is_open()) return 8192; // Default fallback
    std::string line;
    while (std::getline(meminfo, line)) {
        if (line.find("MemAvailable:") == 0) {
            long kb = 0;
            sscanf(line.c_str(), "MemAvailable: %ld kB", &kb);
            return kb / 1024;
        }
    }
    return 8192;
}

bool SystemResourceManager::hasEnoughMemory(long memory_mb) const {
    long available = getAvailableMemoryMb();
    return available >= (memory_mb + MEMORY_HEADROOM_MB);
}

std::vector<std::string> SystemResourceManager::getProcessesToEvict(long required_mb) const {
    std::shared_lock lock(mutex_);
    std::vector<std::string> to_evict;

    if (hasEnoughMemory(required_mb)) return to_evict;

    // Collect idle processes sorted by LRU (oldest last_used first)
    std::vector<ProcessInfo> idle_processes;
    for (const auto& [id, info] : processes_) {
        if (!info.is_active) idle_processes.push_back(info);
    }
    std::sort(idle_processes.begin(), idle_processes.end(),
              [](const ProcessInfo& a, const ProcessInfo& b) {
                  return a.last_used_timestamp < b.last_used_timestamp;
              });

    long freed_mb = 0;
    long available = getAvailableMemoryMb();
    for (const auto& proc : idle_processes) {
        to_evict.push_back(proc.process_id);
        freed_mb += proc.memory_mb;
        if ((available + freed_mb) >= (required_mb + MEMORY_HEADROOM_MB)) break;
    }

    if ((available + freed_mb) < (required_mb + MEMORY_HEADROOM_MB)) {
        LOG_WARN("[SystemResourceManager] Insufficient memory even after evicting all idle processes. "
                 << "Required: " << required_mb << "MB, Available: " << available << "MB");
    }

    return to_evict;
}

void SystemResourceManager::registerProcess(const std::string& process_id,
                                             const std::string& model_name,
                                             long memory_mb, bool is_active) {
    std::unique_lock lock(mutex_);
    ProcessInfo info;
    info.process_id = process_id;
    info.model_name = model_name;
    info.memory_mb = memory_mb;
    info.is_active = is_active;
    info.last_used_timestamp = std::chrono::system_clock::now().time_since_epoch().count();
    processes_[process_id] = info;
    LOG_INFO("[SystemResourceManager] Registered process: " << process_id
             << " (" << model_name << ", " << memory_mb << "MB)");
}

void SystemResourceManager::unregisterProcess(const std::string& process_id) {
    std::unique_lock lock(mutex_);
    processes_.erase(process_id);
}

void SystemResourceManager::markProcessActive(const std::string& process_id) {
    std::unique_lock lock(mutex_);
    auto it = processes_.find(process_id);
    if (it != processes_.end()) {
        it->second.is_active = true;
        it->second.last_used_timestamp = std::chrono::system_clock::now().time_since_epoch().count();
    }
}

void SystemResourceManager::markProcessIdle(const std::string& process_id) {
    std::unique_lock lock(mutex_);
    auto it = processes_.find(process_id);
    if (it != processes_.end()) {
        it->second.is_active = false;
        it->second.last_used_timestamp = std::chrono::system_clock::now().time_since_epoch().count();
    }
}

bool SystemResourceManager::isProcessRegistered(const std::string& process_id) const {
    std::shared_lock lock(mutex_);
    return processes_.count(process_id) > 0;
}
