// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "admin/DiskSpaceGuard.h"

#include <drogon/drogon.h>

#include <cstdlib>
#include <stdexcept>

namespace fs = std::filesystem;

namespace admin {
namespace {

constexpr int64_t kDefaultMinFreeSpaceMb = 1024;

int64_t minFreeSpaceBytes() {
    const char* env = std::getenv("QAISERVE_MIN_FREE_SPACE_MB");
    int64_t mb = kDefaultMinFreeSpaceMb;
    if (env && env[0] != '\0') {
        try {
            mb = std::stoll(env);
            if (mb < 0) mb = kDefaultMinFreeSpaceMb;
        } catch (...) {
            LOG_WARN << "[DiskSpaceGuard] Invalid QAISERVE_MIN_FREE_SPACE_MB='"
                     << env << "', defaulting to " << kDefaultMinFreeSpaceMb;
            mb = kDefaultMinFreeSpaceMb;
        }
    }
    return mb * 1024LL * 1024LL;
}

void requireFreeSpace(const fs::path& path,
                      uint64_t required_bytes,
                      const std::string& label) {
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec) {
        throw std::runtime_error(
            "Failed to create/check " + label + " directory '" + path.string() +
            "': " + ec.message());
    }

    auto info = fs::space(path, ec);
    if (ec) {
        throw std::runtime_error(
            "Failed to read free space for " + label + " directory '" +
            path.string() + "': " + ec.message());
    }
    if (info.available < required_bytes) {
        throw std::runtime_error(
            "Insufficient free space in " + label + " directory '" + path.string() +
            "': available=" + std::to_string(info.available) +
            " bytes, required=" + std::to_string(required_bytes) + " bytes");
    }
}

} // namespace

void requireMinimumFreeSpace(const fs::path& path, const std::string& label) {
    requireFreeSpace(path, static_cast<uint64_t>(minFreeSpaceBytes()), label);
}

void requireAiHubFreeSpace(const fs::path& tmp_dir,
                           const fs::path& models_dir,
                           int64_t zip_bytes) {
    const uint64_t min_free = static_cast<uint64_t>(minFreeSpaceBytes());
    uint64_t zip_size = zip_bytes > 0 ? static_cast<uint64_t>(zip_bytes) : 0;
    requireFreeSpace(tmp_dir, min_free + zip_size, "QAISERVE_TMP_DIR");
    requireFreeSpace(models_dir, min_free + (zip_size * 2), "GENAI_MODELS_DIR");
}

} // namespace admin
