// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace admin {

using LockJson = nlohmann::ordered_json;

// ── Startup cleanup ────────────────────────────────────────────────────────
// Call once at startup to remove qaiserve-owned orphaned download/use locks
// and partial ZIPs left by a previous crash. Locks owned by other services
// sharing GENAI_MODELS_DIR are preserved.
void cleanupOnStartup(const std::string& models_dir, const std::string& tmp_dir);

std::filesystem::path downloadLockPath(const std::string& models_dir,
                                       const std::string& source,
                                       const std::string& model,
                                       const std::string& runtime,
                                       const std::string& precision,
                                       const std::string& chipset);

LockJson buildNewDownloadLockJson(const std::string& models_dir,
                                  const std::string& job_id,
                                  const std::string& source,
                                  const std::string& model,
                                  const std::string& runtime,
                                  const std::string& precision,
                                  const std::string& version,
                                  const std::string& chipset,
                                  const std::string& status);

LockJson buildUpdatedDownloadLockJson(const std::filesystem::path& path,
                                      const std::string& models_dir,
                                      const std::string& job_id,
                                      const std::string& source,
                                      const std::string& model,
                                      const std::string& runtime,
                                      const std::string& precision,
                                      const std::string& version,
                                      const std::string& chipset,
                                      const std::string& status);

// ── Install path helpers ───────────────────────────────────────────────────
// Canonical install path for a model bundle.
// AI Hub: models_dir/{model}-{runtime}-{precision}[-{chipset}]
// GenieX: models_dir/models/{org}/{repo}   (SDK uses GENIEX_DATADIR/models/...)
std::string expectedInstallPath(const std::string& models_dir,
                                const std::string& source,
                                const std::string& model,
                                const std::string& runtime,
                                const std::string& precision,
                                const std::string& chipset);

bool writeJsonFile(const std::filesystem::path& path,
                   const LockJson& body,
                   std::string& error);

bool createDownloadLock(const std::filesystem::path& path,
                        const LockJson& body,
                        std::string& error,
                        LockJson* existing_out = nullptr);

void removeDownloadLock(const std::filesystem::path& path);

void pruneStaleDownloadLocks(const std::string& models_dir);

bool findBlockingLock(const std::string& models_dir,
                      const std::filesystem::path& bundle_path,
                      bool include_use_locks,
                      std::filesystem::path& found_path,
                      LockJson& found_lock);

} // namespace admin
