// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <string>
#include <unordered_map>
#include <deque>
#include <shared_mutex>
#include <chrono>
#include <optional>

// ─────────────────────────────────────────────────────────────────────────────
// MetricsManager — P8: Per-model inference metrics (Section 5 of design)
//
// Tracks moving averages for the last 100 requests per model:
//   - TTFT (Time to First Token) in ms
//   - Tokens per second (steady-state generation rate)
//   - Stream latency (inter-token latency) in ms
//   - Total pipeline latency in ms
//   - Preprocessing time in ms (VLM only)
//
// Thread-safe via shared_mutex (multiple readers, single writer).
// ─────────────────────────────────────────────────────────────────────────────

static constexpr size_t METRICS_WINDOW_SIZE = 100;

struct RequestMetrics {
    std::optional<float> preprocessing_time_ms;  // VLM only
    std::optional<float> ttft_ms;
    std::optional<float> tokens_per_second;
    std::optional<float> stream_latency_ms;
    std::optional<float> total_pipeline_latency_ms;
};

struct ModelMetrics {
    std::optional<float> avg_preprocessing_time_ms;
    std::optional<float> avg_ttft_ms;
    std::optional<float> avg_tokens_per_second;
    std::optional<float> avg_stream_latency_ms;
    std::optional<float> avg_total_pipeline_latency_ms;
};

struct SystemMetrics {
    float cpu_usage_percent = 0.0f;
    float memory_total_gb = 0.0f;
    float memory_used_gb = 0.0f;
    float memory_free_gb = 0.0f;
    float memory_usage_percent = 0.0f;
};

class MetricsManager {
public:
    static MetricsManager& getInstance();

    // ── Record metrics for a completed request ─────────────────────────────────
    void recordRequest(const std::string& model_id, const RequestMetrics& metrics);

    // ── Query API ──────────────────────────────────────────────────────────────
    ModelMetrics getModelMetrics(const std::string& model_id) const;
    std::unordered_map<std::string, ModelMetrics> getAllModelMetrics() const;
    SystemMetrics getSystemMetrics() const;

    // ── Timing helpers (used by ChatOrchestratorImpl) ──────────────────────────
    static std::chrono::steady_clock::time_point now();
    static float elapsedMs(std::chrono::steady_clock::time_point start);

private:
    MetricsManager() = default;
    MetricsManager(const MetricsManager&) = delete;
    MetricsManager& operator=(const MetricsManager&) = delete;

    static float computeAverage(const std::deque<float>& values);

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::deque<RequestMetrics>> model_metrics_;
};
