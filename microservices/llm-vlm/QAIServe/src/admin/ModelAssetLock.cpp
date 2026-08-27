// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "admin/ModelAssetLock.h"

#include <drogon/drogon.h>

#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;

namespace admin {
namespace {

constexpr int64_t kDefaultStaleLockTtlSeconds = 6 * 60 * 60;

int64_t staleLockTtlSeconds() {
    const char* env = std::getenv("QAISERVE_STALE_LOCK_TTL_SECONDS");
    if (!env || env[0] == '\0') return kDefaultStaleLockTtlSeconds;
    try {
        int64_t parsed = std::stoll(env);
        return parsed <= 0 ? kDefaultStaleLockTtlSeconds : parsed;
    } catch (...) {
        LOG_WARN << "[ModelAssetLock] Invalid QAISERVE_STALE_LOCK_TTL_SECONDS='"
                 << env << "', defaulting to " << kDefaultStaleLockTtlSeconds;
        return kDefaultStaleLockTtlSeconds;
    }
}

std::string assetRuntime(const std::string& runtime) {
    if (runtime == "qnn")  return "qnn_context_binary";
    if (runtime == "snpe") return "qnn_dlc";
    return runtime;
}

std::string sanitizeLockName(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '.' || c == '_' || c == '-') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "model" : out;
}

fs::path lockDir(const std::string& models_dir) {
    return fs::path(models_dir) / ".locks";
}

bool isDownloadLockFile(const fs::path& path) {
    std::string name = path.filename().string();
    return name.size() >= 14 && name.find(".download.lock") != std::string::npos;
}

bool isUseLockFile(const fs::path& path) {
    std::string name = path.filename().string();
    return name.size() >= 9 && name.find(".use.") != std::string::npos &&
           name.rfind(".lock") == name.size() - 5;
}

bool loadLockJson(const fs::path& path, LockJson& lock) {
    try {
        std::ifstream in(path);
        lock = LockJson::parse(in);
        return lock.is_object();
    } catch (...) {
        return false;
    }
}

bool isStaleLock(const LockJson& lock, int64_t ttl_seconds) {
    int64_t updated = lock.value("updated_at_unix", int64_t(0));
    if (updated <= 0) updated = lock.value("created_at_unix", int64_t(0));
    if (updated <= 0) return false;
    return static_cast<int64_t>(std::time(nullptr)) - updated > ttl_seconds;
}

LockJson buildDownloadLockJson(const std::string& models_dir,
                               const std::string& job_id,
                               const std::string& source,
                               const std::string& model,
                               const std::string& runtime,
                               const std::string& precision,
                               const std::string& version,
                               const std::string& chipset,
                               const std::string& status,
                               int64_t created_at_unix) {
    std::time_t now = std::time(nullptr);
    LockJson lock = {
        {"owner_service", "qaiserve"},
        {"job_id", job_id},
        {"source", source},
        {"model", model},
        {"runtime", runtime},
        {"precision", precision},
        {"version", version},
        {"target_path", expectedInstallPath(models_dir, source, model, runtime, precision, chipset)},
        {"status", status},
        {"pid", static_cast<int>(getpid())},
        {"created_at_unix", created_at_unix > 0 ? created_at_unix : static_cast<int64_t>(now)},
        {"updated_at_unix", static_cast<int64_t>(now)},
    };
    if (!chipset.empty()) lock["chipset"] = chipset;
    return lock;
}

bool isPathAtOrUnder(const fs::path& candidate, const fs::path& root) {
    fs::path c = candidate.lexically_normal();
    fs::path r = root.lexically_normal();
    if (c == r) return true;

    fs::path rel = c.lexically_relative(r);
    if (rel.empty()) return false;
    auto first = rel.begin();
    return first != rel.end() && *first != "..";
}

} // namespace

std::string expectedInstallPath(const std::string& models_dir,
                                const std::string& source,
                                const std::string& model,
                                const std::string& runtime,
                                const std::string& precision,
                                const std::string& chipset) {
    if (source == "aihub") {
        std::string dest_name = model + "-" + assetRuntime(runtime) + "-" + precision;
        if (!chipset.empty()) dest_name += "-" + chipset;
        return (fs::path(models_dir) / dest_name).string();
    }
    // GenieX SDK writes to GENIEX_DATADIR/models/<org>/<repo>, where GENIEX_DATADIR = models_dir.
    return (fs::path(models_dir) / "models" / model).string();
}

// ── cleanupOnStartup ─────────────────────────────────────────────────────────
// Remove qaiserve-owned .download.lock/.use.lock files (process-scoped,
// invalid after restart) and orphan .partial-* ZIP files left by a previous crash.
void cleanupOnStartup(const std::string& models_dir, const std::string& tmp_dir) {
    std::error_code ec;

    // Remove qaiserve-owned download/use lock files. Do not delete locks owned by
    // other services sharing the same GENAI_MODELS_DIR volume.
    fs::path locks = lockDir(models_dir);
    if (fs::exists(locks, ec)) {
        for (const auto& entry : fs::directory_iterator(locks, ec)) {
            if (!entry.is_regular_file()) continue;
            if (!isDownloadLockFile(entry.path()) && !isUseLockFile(entry.path())) continue;

            LockJson lock;
            if (!loadLockJson(entry.path(), lock)) continue;
            if (lock.value("owner_service", "") != "qaiserve") continue;

            fs::remove(entry.path(), ec);
            if (ec) {
                LOG_WARN << "[ModelAssetLock] cleanupOnStartup: failed to remove "
                         << entry.path().string() << ": " << ec.message();
            } else {
                LOG_INFO << "[ModelAssetLock] cleanupOnStartup: removed stale lock "
                         << entry.path().string();
            }
        }
    }

    // Remove orphan partial ZIP files
    fs::path tmp = fs::path(tmp_dir);
    if (fs::exists(tmp, ec)) {
        for (const auto& entry : fs::directory_iterator(tmp, ec)) {
            if (!entry.is_regular_file()) continue;
            std::string name = entry.path().filename().string();
            if (name.find(".partial-") != std::string::npos) {
                fs::remove(entry.path(), ec);
                if (ec) {
                    LOG_WARN << "[ModelAssetLock] cleanupOnStartup: failed to remove "
                             << entry.path().string() << ": " << ec.message();
                } else {
                    LOG_INFO << "[ModelAssetLock] cleanupOnStartup: removed orphan partial "
                             << entry.path().string();
                }
            }
        }
    }
}

fs::path downloadLockPath(const std::string& models_dir,
                          const std::string& source,
                          const std::string& model,
                          const std::string& runtime,
                          const std::string& precision,
                          const std::string& chipset) {
    std::string target = expectedInstallPath(models_dir, source, model, runtime, precision, chipset);
    std::string key = source + "-" + target;
    return lockDir(models_dir) / (sanitizeLockName(key) + ".download.lock");
}

LockJson buildNewDownloadLockJson(const std::string& models_dir,
                                  const std::string& job_id,
                                  const std::string& source,
                                  const std::string& model,
                                  const std::string& runtime,
                                  const std::string& precision,
                                  const std::string& version,
                                  const std::string& chipset,
                                  const std::string& status) {
    return buildDownloadLockJson(
        models_dir, job_id, source, model, runtime, precision, version, chipset,
        status, static_cast<int64_t>(std::time(nullptr)));
}

LockJson buildUpdatedDownloadLockJson(const fs::path& path,
                                      const std::string& models_dir,
                                      const std::string& job_id,
                                      const std::string& source,
                                      const std::string& model,
                                      const std::string& runtime,
                                      const std::string& precision,
                                      const std::string& version,
                                      const std::string& chipset,
                                      const std::string& status) {
    LockJson existing;
    int64_t created_at = 0;
    if (loadLockJson(path, existing)) {
        created_at = existing.value("created_at_unix", int64_t(0));
    }
    return buildDownloadLockJson(
        models_dir, job_id, source, model, runtime, precision, version, chipset,
        status, created_at);
}

bool writeJsonFile(const fs::path& path, const LockJson& body, std::string& error) {
    try {
        std::ofstream out(path);
        if (!out) {
            error = "failed to open " + path.string();
            return false;
        }
        out << body.dump(2);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool createDownloadLock(const fs::path& path, const LockJson& body, std::string& error,
                        LockJson* existing_out) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "failed to create lock directory: " + ec.message();
        return false;
    }

    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        if (errno == EEXIST) {
            LockJson existing;
            bool loaded = loadLockJson(path, existing);
            if (loaded && existing_out) *existing_out = existing;
            error = loaded
                        ? "model download is already locked: " + existing.dump()
                        : "model download is already locked by " + path.string();
        } else {
            error = "failed to create lock " + path.string() + ": " + std::strerror(errno);
        }
        return false;
    }

    std::string payload = body.dump(2);
    ssize_t written = write(fd, payload.data(), payload.size());
    close(fd);
    if (written < 0 || static_cast<size_t>(written) != payload.size()) {
        fs::remove(path, ec);
        error = "failed to write lock " + path.string();
        return false;
    }
    return true;
}

void removeDownloadLock(const fs::path& path) {
    std::error_code ec;
    fs::remove(path, ec);
    if (ec) {
        LOG_WARN << "[ModelAssetLock] Failed to remove download lock "
                 << path.string() << ": " << ec.message();
    }
}

void pruneStaleDownloadLocks(const std::string& models_dir) {
    fs::path locks = lockDir(models_dir);
    std::error_code ec;
    if (!fs::exists(locks, ec)) return;

    const int64_t ttl = staleLockTtlSeconds();
    for (const auto& entry : fs::directory_iterator(locks, ec)) {
        if (!entry.is_regular_file()) continue;
        if (!isDownloadLockFile(entry.path())) continue;

        LockJson lock;
        if (!loadLockJson(entry.path(), lock)) continue;
        if (!isStaleLock(lock, ttl)) continue;

        fs::remove(entry.path(), ec);
        if (ec) {
            LOG_WARN << "[ModelAssetLock] Failed to prune stale download lock "
                     << entry.path().string() << ": " << ec.message();
        } else {
            LOG_WARN << "[ModelAssetLock] Pruned stale download lock "
                     << entry.path().string() << " lock=" << lock.dump();
        }
    }
}

bool findBlockingLock(const std::string& models_dir,
                      const fs::path& bundle_path,
                      bool include_use_locks,
                      fs::path& found_path,
                      LockJson& found_lock) {
    fs::path locks = lockDir(models_dir);
    std::error_code ec;
    if (!fs::exists(locks, ec)) return false;

    fs::directory_iterator it(locks, ec);
    if (ec) {
        LOG_WARN << "[ModelAssetLock] findBlockingLock: failed to open lock dir "
                 << locks.string() << ": " << ec.message();
        return false;
    }

    for (const auto& entry : it) {
        if (!entry.is_regular_file()) continue;
        if (!isDownloadLockFile(entry.path()) &&
            !(include_use_locks && isUseLockFile(entry.path()))) {
            continue;
        }

        LockJson lock;
        if (!loadLockJson(entry.path(), lock)) continue;
        std::string target = lock.value("target_path", "");
        if (target.empty()) continue;
        if (isPathAtOrUnder(fs::path(target), bundle_path) ||
            isPathAtOrUnder(bundle_path, fs::path(target))) {
            found_path = entry.path();
            found_lock = lock;
            return true;
        }
    }
    return false;
}

} // namespace admin
