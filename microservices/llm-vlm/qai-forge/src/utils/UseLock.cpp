// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/utils/UseLock.h"
#include "qai_forge/managers/ModelConfigManager.h"
#include "qai_forge/utils/Logger.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

namespace qai_forge {
namespace {

std::string sanitize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
        out.push_back((std::isalnum(c) || c == '.' || c == '_' || c == '-')
                          ? static_cast<char>(c) : '_');
    return out.empty() ? "model" : out;
}

std::string modelsDir() {
    const char* models_env = std::getenv("GENAI_MODELS_DIR");
    return (models_env && models_env[0] != '\0') ? models_env : "/mnt/work/models";
}

fs::path useLockPath(const std::string& model_id) {
    return fs::path(modelsDir()) / ".locks" /
           ("qaiserve." + sanitize(model_id) + ".use.lock");
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

fs::path targetPathForModel(const std::string& model_id) {
    const ModelConfig* config = ModelConfigManager::getInstance().getModelConfig(model_id);
    if (!config || config->config_file.empty()) return {};

    fs::path config_path = fs::path(config->config_file).lexically_normal();
    fs::path models_path = fs::path(modelsDir()).lexically_normal();
    if (isPathAtOrUnder(config_path, models_path)) {
        return config_path.parent_path();
    }

    // Generative metadata bundles often use processed configs under
    // /tmp/configs/<bundle_name>/...; map back to GENAI_MODELS_DIR/<bundle_name>.
    fs::path tmp_configs = fs::path("/tmp/configs").lexically_normal();
    if (isPathAtOrUnder(config_path, tmp_configs)) {
        fs::path rel = config_path.lexically_relative(tmp_configs);
        auto it = rel.begin();
        if (it != rel.end() && *it != "..") {
            return models_path / *it;
        }
    }

    return {};
}

} // namespace

void writeUseLock(const std::string& model_id) {
    fs::path path = useLockPath(model_id);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        LOG_WARN("[UseLock] Failed to create lock dir for " << model_id
                 << ": " << ec.message());
        return;
    }

    fs::path target_path = targetPathForModel(model_id);
    if (target_path.empty()) {
        LOG_WARN("[UseLock] Could not resolve bundle path for " << model_id
                 << "; DELETE protection will not be installed");
        return;
    }

    std::time_t now = std::time(nullptr);
    json lock = {
        {"owner_service", "qaiserve"},
        {"model_id", model_id},
        {"target_path", target_path.string()},
        {"pid", static_cast<int>(getpid())},
        {"created_at_unix", static_cast<int64_t>(now)},
        {"updated_at_unix", static_cast<int64_t>(now)},
    };

    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        LOG_WARN("[UseLock] Failed to open lock file for " << model_id
                 << ": " << std::strerror(errno));
        return;
    }
    std::string payload = lock.dump(2);
    ssize_t written = write(fd, payload.data(), payload.size());
    close(fd);
    if (written < 0 || static_cast<size_t>(written) != payload.size()) {
        LOG_WARN("[UseLock] Failed to write lock file for " << model_id);
    }
}

void removeUseLock(const std::string& model_id) {
    fs::path path = useLockPath(model_id);
    std::error_code ec;
    fs::remove(path, ec);
    // Silently ignore ENOENT — model may not have acquired a lock
}

} // namespace qai_forge
