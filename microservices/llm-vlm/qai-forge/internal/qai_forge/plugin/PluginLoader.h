// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// PluginLoader — dlopen-based discovery/loading of qai-forge plugins
//
// Scans a directory for shared libraries, dlopen()'s each one, and resolves
// its "qai_forge_register_plugin" extern "C" entry point, calling it with
// the process-wide PluginRegistry singleton so the plugin can register
// whichever capability (backend, model adapter, vision adapter) it provides.
//
// A plugin that fails to load or resolve its entry point is logged and
// skipped — never fatal to process startup.
// ─────────────────────────────────────────────────────────────────────────────

class PluginLoader {
public:
    /**
     * Discover and load every plugin shared library in plugin_dir.
     *
     * @param plugin_dir  Directory to scan for plugin *.so files. If empty,
     *                    resolvePluginDir() is used to determine the
     *                    effective directory (QAI_FORGE_PLUGIN_DIR env var,
     *                    falling back to the installed plugins directory).
     * @return Number of plugins successfully loaded and registered.
     */
    static int loadAll(const std::string& plugin_dir = "");

    /**
     * Resolve the effective plugin directory:
     *   1. QAI_FORGE_PLUGIN_DIR environment variable, if set.
     *   2. <install-prefix>/lib/qai-forge/plugins (compile-time default).
     */
    static std::string resolvePluginDir();

private:
    PluginLoader() = delete;
};
