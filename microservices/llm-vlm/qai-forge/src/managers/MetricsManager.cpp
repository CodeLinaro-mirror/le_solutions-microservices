// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/managers/MetricsManager.h"
#include <fstream>
#include <sstream>
#include <numeric>
#include <mutex>

MetricsManager& MetricsManager::getInstance() {
    static MetricsManager instance;
    return instance;
}

std::chrono::steady_clock::time_point MetricsManager::now() {
    return std::chrono::steady_clock::now();
}

float MetricsManager::elapsedMs(std::chrono::steady_clock::time_point start) {
    auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<float, std::milli>(end - start).count();
}

float MetricsManager::computeAverage(const std::deque<float>& values) {
    if (values.empty()) return 0.0f;
    float sum = std::accumulate(values.begin(), values.end(), 0.0f);
    return sum / static_cast<float>(values.size());
}

void MetricsManager::recordRequest(const std::string& model_id, const RequestMetrics& metrics) {
    std::unique_lock lock(mutex_);
    auto& window = model_metrics_[model_id];
    window.push_back(metrics);
    if (window.size() > METRICS_WINDOW_SIZE) {
        window.pop_front();
    }
}

ModelMetrics MetricsManager::getModelMetrics(const std::string& model_id) const {
    std::shared_lock lock(mutex_);
    ModelMetrics result;
    auto it = model_metrics_.find(model_id);
    if (it == model_metrics_.end() || it->second.empty()) return result;

    const auto& window = it->second;
    std::deque<float> preprocessing, ttft, tps, stream_lat, total_lat;

    for (const auto& m : window) {
        if (m.preprocessing_time_ms.has_value()) preprocessing.push_back(m.preprocessing_time_ms.value());
        if (m.ttft_ms.has_value()) ttft.push_back(m.ttft_ms.value());
        if (m.tokens_per_second.has_value()) tps.push_back(m.tokens_per_second.value());
        if (m.stream_latency_ms.has_value()) stream_lat.push_back(m.stream_latency_ms.value());
        if (m.total_pipeline_latency_ms.has_value()) total_lat.push_back(m.total_pipeline_latency_ms.value());
    }

    if (!preprocessing.empty()) result.avg_preprocessing_time_ms = computeAverage(preprocessing);
    if (!ttft.empty()) result.avg_ttft_ms = computeAverage(ttft);
    if (!tps.empty()) result.avg_tokens_per_second = computeAverage(tps);
    if (!stream_lat.empty()) result.avg_stream_latency_ms = computeAverage(stream_lat);
    if (!total_lat.empty()) result.avg_total_pipeline_latency_ms = computeAverage(total_lat);

    return result;
}

std::unordered_map<std::string, ModelMetrics> MetricsManager::getAllModelMetrics() const {
    std::shared_lock lock(mutex_);
    std::unordered_map<std::string, ModelMetrics> result;
    for (const auto& [model_id, _] : model_metrics_) {
        result[model_id] = getModelMetrics(model_id);
    }
    return result;
}

SystemMetrics MetricsManager::getSystemMetrics() const {
    SystemMetrics metrics;
    // Read from /proc/meminfo
    std::ifstream meminfo("/proc/meminfo");
    if (meminfo.is_open()) {
        std::string line;
        long mem_total_kb = 0, mem_free_kb = 0, mem_available_kb = 0;
        while (std::getline(meminfo, line)) {
            if (line.find("MemTotal:") == 0) sscanf(line.c_str(), "MemTotal: %ld kB", &mem_total_kb);
            else if (line.find("MemFree:") == 0) sscanf(line.c_str(), "MemFree: %ld kB", &mem_free_kb);
            else if (line.find("MemAvailable:") == 0) sscanf(line.c_str(), "MemAvailable: %ld kB", &mem_available_kb);
        }
        metrics.memory_total_gb = static_cast<float>(mem_total_kb) / (1024.0f * 1024.0f);
        metrics.memory_free_gb = static_cast<float>(mem_available_kb) / (1024.0f * 1024.0f);
        metrics.memory_used_gb = metrics.memory_total_gb - metrics.memory_free_gb;
        if (metrics.memory_total_gb > 0)
            metrics.memory_usage_percent = (metrics.memory_used_gb / metrics.memory_total_gb) * 100.0f;
    }

    // Read CPU usage from /proc/stat
    std::ifstream stat("/proc/stat");
    if (stat.is_open()) {
        std::string line;
        std::getline(stat, line);
        long user, nice, system, idle, iowait, irq, softirq;
        sscanf(line.c_str(), "cpu %ld %ld %ld %ld %ld %ld %ld",
               &user, &nice, &system, &idle, &iowait, &irq, &softirq);
        long total = user + nice + system + idle + iowait + irq + softirq;
        long active = total - idle - iowait;
        if (total > 0)
            metrics.cpu_usage_percent = (static_cast<float>(active) / static_cast<float>(total)) * 100.0f;
    }

    return metrics;
}
