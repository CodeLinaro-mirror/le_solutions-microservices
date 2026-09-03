// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "shm/SharedMemoryManager.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

using json = nlohmann::json;

namespace {

// Builds a SharedMemoryRegion whose deleter unmaps mmap_base/mmap_length —
// fires once the last shared_ptr reference (registry's own + any in-flight
// request holding a copy via getRegion()) is released, so unregister() while
// a request is mid-memcpy safely defers the actual munmap.
std::shared_ptr<SharedMemoryRegion> makeRegion(
    std::string name, std::string shm_key, size_t offset, size_t byte_size,
    void* mapped_addr, void* mmap_base, size_t mmap_length) {
    auto* raw = new SharedMemoryRegion{
        std::move(name), std::move(shm_key), offset, byte_size,
        mapped_addr, mmap_base, mmap_length};
    return std::shared_ptr<SharedMemoryRegion>(raw, [](SharedMemoryRegion* r) {
        if (r->mmap_base != nullptr && r->mmap_base != MAP_FAILED) {
            munmap(r->mmap_base, r->mmap_length);
        }
        delete r;
    });
}

} // namespace

SharedMemoryManager& SharedMemoryManager::getInstance() {
    static SharedMemoryManager instance;
    return instance;
}

void SharedMemoryManager::registerRegion(const std::string& name, const std::string& shm_key,
                                          size_t offset, size_t byte_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (regions_.count(name)) {
        throw std::runtime_error("Shared memory region '" + name + "' is already registered");
    }

    int fd = shm_open(shm_key.c_str(), O_RDWR, 0);
    if (fd < 0) {
        throw std::runtime_error("Failed to shm_open key '" + shm_key + "': " + std::strerror(errno));
    }

    struct stat st{};
    if (fstat(fd, &st) != 0) {
        int err = errno;
        close(fd);
        throw std::runtime_error("Failed to fstat shm key '" + shm_key + "': " + std::strerror(err));
    }
    size_t segment_size = static_cast<size_t>(st.st_size);
    if (offset + byte_size > segment_size) {
        close(fd);
        throw std::runtime_error(
            "Region [" + std::to_string(offset) + ", " + std::to_string(offset + byte_size) +
            ") exceeds shm segment '" + shm_key + "' size " + std::to_string(segment_size));
    }

    long page_size = sysconf(_SC_PAGESIZE);
    size_t aligned_offset = (offset / static_cast<size_t>(page_size)) * static_cast<size_t>(page_size);
    size_t adjust = offset - aligned_offset;
    size_t map_length = byte_size + adjust;

    void* base = mmap(nullptr, map_length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, static_cast<off_t>(aligned_offset));
    int mmap_err = errno;
    close(fd); // fd not needed after mmap — the mapping stays valid
    if (base == MAP_FAILED) {
        throw std::runtime_error("Failed to mmap shm key '" + shm_key + "': " + std::strerror(mmap_err));
    }

    void* mapped_addr = static_cast<char*>(base) + adjust;
    regions_.emplace(name, makeRegion(name, shm_key, offset, byte_size, mapped_addr, base, map_length));
}

std::shared_ptr<const SharedMemoryRegion> SharedMemoryManager::getRegion(
    const std::string& name, size_t offset, size_t byte_size) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = regions_.find(name);
    if (it == regions_.end()) {
        throw std::runtime_error("Shared memory region '" + name + "' is not registered");
    }
    if (offset + byte_size > it->second->byte_size) {
        throw std::runtime_error(
            "Requested range [" + std::to_string(offset) + ", " + std::to_string(offset + byte_size) +
            ") exceeds registered region '" + name + "' size " + std::to_string(it->second->byte_size));
    }
    return it->second;
}

json SharedMemoryManager::getStatus(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto toJson = [](const SharedMemoryRegion& r) {
        return json{
            {"name",      r.name},
            {"key",       r.shm_key},
            {"offset",    r.offset},
            {"byte_size", r.byte_size},
        };
    };
    if (name.empty()) {
        json regions = json::array();
        for (const auto& [region_name, region] : regions_) {
            regions.push_back(toJson(*region));
        }
        return json{{"regions", regions}};
    }
    auto it = regions_.find(name);
    if (it == regions_.end()) {
        throw std::runtime_error("Shared memory region '" + name + "' is not registered");
    }
    return toJson(*it->second);
}

void SharedMemoryManager::unregister(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (name.empty()) {
        regions_.clear();
        return;
    }
    auto it = regions_.find(name);
    if (it == regions_.end()) {
        throw std::runtime_error("Shared memory region '" + name + "' is not registered");
    }
    regions_.erase(it);
}
