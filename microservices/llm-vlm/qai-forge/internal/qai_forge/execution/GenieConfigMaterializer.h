// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#ifdef QAI_FORGE_BUILD_GENIE

#include "qai_forge/execution/ExecutionPlan.h"
#include "qai_forge/execution/RuntimeArtifacts.h"

#include <filesystem>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace qai_forge {
namespace execution {

// Native GenIE launch configuration produced for one runtime instance.
// The returned RuntimeArtifacts owner must be retained until the worker has
// terminated and been reaped.
struct MaterializedLaunchSpec {
    RuntimeId runtime_id;
    std::string provider = "genie";
    std::filesystem::path config_file;
    std::filesystem::path sampler_config_file;
    nlohmann::ordered_json native_options;
    std::shared_ptr<RuntimeArtifacts> artifacts;
};

// Copies an immutable discovered GenIE JSON graph into a private,
// runtime-specific artifact tree and patches GenIE HTP extension JSON.
class GenieConfigMaterializer {
public:
    struct Options {
        std::filesystem::path artifact_parent =
            "/tmp/qai-forge/runtimes";
        // Root of the immutable discovered bundle containing config_file.
        // When empty, config_file's parent directory is used.
        std::filesystem::path source_root;
        std::string provider = "genie";
    };

    GenieConfigMaterializer();
    explicit GenieConfigMaterializer(Options options);

    // Materialize one GenIE launch. Throws std::runtime_error before
    // returning if a graph reference is malformed, missing, unsafe, or
    // cannot be published atomically.
    MaterializedLaunchSpec materialize(
        const std::filesystem::path& config_file,
        const std::filesystem::path& sampler_file,
        const RuntimeId& runtime_id,
        int selected_device_id,
        RuntimeArtifacts::GenerationToken generation = {}) const;

private:
    Options options_;
};

}  // namespace execution
}  // namespace qai_forge

#endif // QAI_FORGE_BUILD_GENIE
