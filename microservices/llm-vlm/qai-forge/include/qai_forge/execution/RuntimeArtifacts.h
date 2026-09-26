// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <filesystem>
#include <memory>
#include <utility>

namespace qai_forge {
namespace execution {

// Owns runtime-specific generated files and directories for one runtime
// backend adapter. Artifacts are removed only when the final shared owner
// is destroyed. The optional generation token allows a runtime artifact to
// retain the immutable discovery generation from which it was materialized.
class RuntimeArtifacts {
public:
    using GenerationToken = std::shared_ptr<const void>;

    static std::shared_ptr<RuntimeArtifacts> create(
        std::filesystem::path root,
        GenerationToken generation = {});

    ~RuntimeArtifacts();

    RuntimeArtifacts(const RuntimeArtifacts&) = delete;
    RuntimeArtifacts& operator=(const RuntimeArtifacts&) = delete;

    const std::filesystem::path& root() const noexcept { return root_; }
    bool ownsRoot() const noexcept { return owns_root_; }

private:
    RuntimeArtifacts(std::filesystem::path root,
                     GenerationToken generation);

    std::filesystem::path root_;
    GenerationToken generation_;
    bool owns_root_ = false;
};

}  // namespace execution
}  // namespace qai_forge
