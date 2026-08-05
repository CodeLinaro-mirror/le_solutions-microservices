// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace admin {

void requireMinimumFreeSpace(const std::filesystem::path& path,
                             const std::string& label);

void requireAiHubFreeSpace(const std::filesystem::path& tmp_dir,
                           const std::filesystem::path& models_dir,
                           int64_t zip_bytes);

} // namespace admin
