// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// SharedMemoryManager — client-facing POSIX system shared memory registry
//
// Mirrors the mechanism NVIDIA Triton Inference Server exposes at
// /v2/systemsharedmemory: a same-host client creates a POSIX named shared
// memory segment (shm_open), writes tensor/image bytes into it, and
// registers the region with QAIServe by name. Subsequent /infer or
// /generate requests reference the region by name instead of embedding the
// bytes in the HTTP body — QAIServe resolves the reference to a mapped
// pointer and does a single local memcpy into the existing owning
// InputTensor::data / image buffer (see OipBinaryParser / InferController).
// The same region can also be referenced as a *requested output* on
// /infer — QAIServe memcpy's the OutputTensor bytes into the region after
// inference instead of returning them as a JSON "data" array.
//
// This requires the QAIServe container's /dev/shm to be bind-mounted from
// the host (see docker-compose.yaml) so shm_open() by key resolves to the
// same tmpfs instance on both sides — Docker's default private per-container
// tmpfs would make registered names invisible to the server.
//
// Regions are opened O_RDWR / mapped PROT_READ|PROT_WRITE — one registration
// path serves both directions, same as Triton's shared_memory_manager.cc.
// A region only used as an input is never written to; nothing forces a
// write unless a request references it as a requested output.
//
// Disabled by default. Enable with QAISERVE_ALLOW_CLIENT_SHM=1 (main.cpp)
// — a same-host client with a valid region name can make the server
// shm_open() any key it names (and, now, write into it), so this is a
// same-host trust boundary, gated independently of QAISERVE_ADMIN_TOKEN (a
// different concern: model management vs. request-time memory access).
// ─────────────────────────────────────────────────────────────────────────────

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <nlohmann/json.hpp>

struct SharedMemoryRegion {
    std::string name;
    std::string shm_key;
    size_t      offset;      // byte offset into the shm segment this region starts at
    size_t      byte_size;   // registered region size, in bytes
    void*       mapped_addr; // pointer to `offset` within the mmap'd segment
    void*       mmap_base;   // raw mmap() base (page-aligned; needed for munmap)
    size_t      mmap_length; // length passed to munmap()
};

class SharedMemoryManager {
public:
    static SharedMemoryManager& getInstance();

    // Opens shm_key (POSIX named shared memory) and mmaps [offset, offset+byte_size).
    // Throws std::runtime_error if shm_key can't be opened, the region is out
    // of bounds for the underlying segment, or `name` is already registered.
    void registerRegion(const std::string& name, const std::string& shm_key,
                         size_t offset, size_t byte_size);

    // Resolves `name` and validates that [offset, offset+byte_size) fits
    // within the region registered under that name. Returns a shared_ptr so
    // an in-flight request keeps the mapping alive even if unregister() is
    // called concurrently — the mapping is only munmap'd once the last
    // reference (registry's own + any in-flight requests) is released.
    // Throws std::runtime_error if `name` is not registered or the requested
    // range is out of bounds.
    std::shared_ptr<const SharedMemoryRegion> getRegion(
        const std::string& name, size_t offset, size_t byte_size) const;

    // Status for one region, or all regions if `name` is empty.
    // Throws std::runtime_error if `name` is non-empty and not registered.
    nlohmann::json getStatus(const std::string& name) const;

    // Unregisters one region, or all regions if `name` is empty. munmap()
    // happens once the shared_ptr's last reference (including any in-flight
    // request currently reading it) is released.
    // Throws std::runtime_error if `name` is non-empty and not registered.
    void unregister(const std::string& name);

    bool allowClientShm() const { return allow_client_shm_; }
    void setAllowClientShm(bool value) { allow_client_shm_ = value; }

private:
    SharedMemoryManager() = default;
    ~SharedMemoryManager() = default;
    SharedMemoryManager(const SharedMemoryManager&) = delete;
    SharedMemoryManager& operator=(const SharedMemoryManager&) = delete;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<SharedMemoryRegion>> regions_;
    bool allow_client_shm_ = false;
};
