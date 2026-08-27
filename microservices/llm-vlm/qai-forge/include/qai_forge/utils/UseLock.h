// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
#pragma once
#include <string>

namespace qai_forge {

// Write a qaiserve.<model_id>.use.lock file under GENAI_MODELS_DIR/.locks/
// so DELETE is blocked while the model is loaded for inference. The lock
// target_path resolves to the concrete bundle directory, not the full models dir.
// Non-fatal: failures are logged but do not affect inference.
void writeUseLock(const std::string& model_id);

// Remove the use lock written by writeUseLock().
void removeUseLock(const std::string& model_id);

} // namespace qai_forge
