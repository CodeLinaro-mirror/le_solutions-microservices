// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/plugin/PluginLoader.h"
#include "qai_forge/plugin/PluginRegistry.h"
#include "qai_forge/utils/Logger.h"

#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include <dirent.h>
#include <dlfcn.h>

namespace {

// Compile-time default install location for plugin shared libraries.
// Overridable at build time via -DQAI_FORGE_PLUGIN_DIR_DEFAULT=<path>.
#ifndef QAI_FORGE_PLUGIN_DIR_DEFAULT
#define QAI_FORGE_PLUGIN_DIR_DEFAULT "/usr/lib/qai-forge/plugins"
#endif

using RegisterPluginFn = void (*)(PluginRegistry&);

// Keeps every successfully dlopen()'d handle alive for the lifetime of the
// process — plugin vtables/static state must stay valid for as long as any
// backend/orchestrator/adapter instance created from that plugin might be
// alive.
std::vector<void*>& openPluginHandles() {
    static std::vector<void*> handles;
    return handles;
}

std::mutex& openPluginHandlesMutex() {
    static std::mutex mutex;
    return mutex;
}

bool hasSharedLibraryExtension(const std::string& filename) {
    const std::string suffix = ".so";
    if (filename.size() < suffix.size()) {
        return false;
    }
    return filename.compare(filename.size() - suffix.size(),
                            suffix.size(), suffix) == 0;
}

bool loadOnePlugin(const std::string& path) {
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char* error = dlerror();
        LOG_WARN("[PluginLoader] Failed to dlopen plugin: " << path
                 << " error=\"" << (error ? error : "unknown") << "\"");
        return false;
    }

    dlerror(); // clear any stale error state before dlsym
    void* symbol = dlsym(handle, "qai_forge_register_plugin");
    const char* symbol_error = dlerror();
    if (!symbol || symbol_error) {
        LOG_WARN("[PluginLoader] Plugin missing qai_forge_register_plugin "
                 "entry point: " << path
                 << " error=\"" << (symbol_error ? symbol_error : "not found") << "\"");
        dlclose(handle);
        return false;
    }

    auto register_fn = reinterpret_cast<RegisterPluginFn>(symbol);
    try {
        register_fn(PluginRegistry::instance());
    } catch (const std::exception& e) {
        LOG_WARN("[PluginLoader] Plugin registration threw: " << path
                 << " error=\"" << e.what() << "\"");
        dlclose(handle);
        return false;
    } catch (...) {
        LOG_WARN("[PluginLoader] Plugin registration threw an unknown "
                 "exception: " << path);
        dlclose(handle);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(openPluginHandlesMutex());
        openPluginHandles().push_back(handle);
    }
    LOG_INFO("[PluginLoader] Loaded plugin: " << path);
    return true;
}

} // namespace

std::string PluginLoader::resolvePluginDir() {
    if (const char* env = std::getenv("QAI_FORGE_PLUGIN_DIR")) {
        if (env[0] != '\0') {
            return env;
        }
    }
    return QAI_FORGE_PLUGIN_DIR_DEFAULT;
}

int PluginLoader::loadAll(const std::string& plugin_dir) {
    const std::string dir = plugin_dir.empty() ? resolvePluginDir() : plugin_dir;

    DIR* handle = opendir(dir.c_str());
    if (!handle) {
        LOG_WARN("[PluginLoader] Plugin directory not accessible, skipping "
                 "plugin discovery: " << dir);
        return 0;
    }

    std::vector<std::string> candidate_paths;
    while (dirent* entry = readdir(handle)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        if (!hasSharedLibraryExtension(name)) {
            continue;
        }
        candidate_paths.push_back(dir + "/" + name);
    }
    closedir(handle);

    int loaded_count = 0;
    for (const std::string& path : candidate_paths) {
        if (loadOnePlugin(path)) {
            ++loaded_count;
        }
    }

    LOG_INFO("[PluginLoader] Plugin discovery complete: dir=" << dir
             << " found=" << candidate_paths.size()
             << " loaded=" << loaded_count);
    return loaded_count;
}
