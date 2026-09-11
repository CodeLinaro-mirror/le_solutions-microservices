// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

// ─────────────────────────────────────────────────────────────────────────────
// PredictiveSharedMemory — backend-owned tensor transport memory
//
// Owns the memfd-backed mapping shared with a predictive inference worker.
// The region is split into two equal halves: input (backend writes, worker
// reads) and output (worker writes, backend reads). PredictiveWorkerManager
// borrows the fd, pointer, and per-direction capacity only to fork/exec and
// exchange protocol data; it never closes or unmaps this resource.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Non-owning shared-memory view passed from a backend to its worker manager.
 */
struct PredictiveSharedMemoryView {
    int    fd        = -1;
    void*  ptr       = nullptr;
    size_t dir_bytes = 0;
};

/**
 * Compact worker IPC request prepared after input bytes have been written into
 * backend-owned shared memory. It intentionally contains no raw input buffers.
 */
struct PredictiveExecuteRequest {
    std::string            model;
    std::string            request_id;
    nlohmann::ordered_json inputs;
    nlohmann::ordered_json output_names;
};

class PredictiveSharedMemory {
public:
    explicit PredictiveSharedMemory(size_t dir_bytes);
    ~PredictiveSharedMemory();

    PredictiveSharedMemory(const PredictiveSharedMemory&) = delete;
    PredictiveSharedMemory& operator=(const PredictiveSharedMemory&) = delete;

    PredictiveSharedMemory(PredictiveSharedMemory&& other) noexcept;
    PredictiveSharedMemory& operator=(PredictiveSharedMemory&& other) noexcept;

    int fd() const noexcept { return fd_; }
    uint8_t* data() const noexcept { return data_; }
    size_t dirBytes() const noexcept { return dir_bytes_; }
    size_t totalBytes() const noexcept { return 2 * dir_bytes_; }

private:
    void reset() noexcept;

    int fd_ = -1;
    uint8_t* data_ = nullptr;
    size_t dir_bytes_ = 0;
};