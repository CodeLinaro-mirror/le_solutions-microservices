// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/backend/PredictiveSharedMemory.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

PredictiveSharedMemory::PredictiveSharedMemory(size_t dir_bytes)
    : dir_bytes_(dir_bytes) {
    if (dir_bytes_ == 0) {
        throw std::invalid_argument("PredictiveSharedMemory capacity must be greater than zero");
    }

    // Do not use MFD_CLOEXEC: PredictiveWorkerManager passes this inherited fd
    // to the worker through CONV_SHM_FD during fork()/execl().
    fd_ = memfd_create("qai-forge-predictive-shm", 0);
    if (fd_ < 0) {
        throw std::runtime_error(
            "PredictiveSharedMemory memfd_create failed: " + std::string(std::strerror(errno)));
    }

    if (ftruncate(fd_, static_cast<off_t>(totalBytes())) < 0) {
        const std::string error = std::strerror(errno);
        reset();
        throw std::runtime_error("PredictiveSharedMemory ftruncate failed: " + error);
    }

    void* mapped = mmap(nullptr, totalBytes(), PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (mapped == MAP_FAILED) {
        const std::string error = std::strerror(errno);
        reset();
        throw std::runtime_error("PredictiveSharedMemory mmap failed: " + error);
    }

    data_ = static_cast<uint8_t*>(mapped);
}

PredictiveSharedMemory::~PredictiveSharedMemory() {
    reset();
}

PredictiveSharedMemory::PredictiveSharedMemory(PredictiveSharedMemory&& other) noexcept
    : fd_(other.fd_)
    , data_(other.data_)
    , dir_bytes_(other.dir_bytes_) {
    other.fd_ = -1;
    other.data_ = nullptr;
    other.dir_bytes_ = 0;
}

PredictiveSharedMemory& PredictiveSharedMemory::operator=(PredictiveSharedMemory&& other) noexcept {
    if (this != &other) {
        reset();
        fd_ = other.fd_;
        data_ = other.data_;
        dir_bytes_ = other.dir_bytes_;
        other.fd_ = -1;
        other.data_ = nullptr;
        other.dir_bytes_ = 0;
    }
    return *this;
}

void PredictiveSharedMemory::reset() noexcept {
    if (data_ != nullptr) {
        munmap(data_, totalBytes());
        data_ = nullptr;
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    dir_bytes_ = 0;
}