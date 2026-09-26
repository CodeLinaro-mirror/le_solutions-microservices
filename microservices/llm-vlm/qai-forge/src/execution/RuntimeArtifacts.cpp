// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/execution/RuntimeArtifacts.h"

#include <system_error>

namespace qai_forge {
namespace execution {

std::shared_ptr<RuntimeArtifacts> RuntimeArtifacts::create(
    std::filesystem::path root,
    GenerationToken generation) {
    return std::shared_ptr<RuntimeArtifacts>(
        new RuntimeArtifacts(std::move(root), std::move(generation)));
}

RuntimeArtifacts::RuntimeArtifacts(std::filesystem::path root,
                                   GenerationToken generation)
    : root_(std::move(root)),
      generation_(std::move(generation)),
      owns_root_(!root_.empty()) {}

RuntimeArtifacts::~RuntimeArtifacts() {
    if (!owns_root_) {
        return;
    }

    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
}

}  // namespace execution
}  // namespace qai_forge
