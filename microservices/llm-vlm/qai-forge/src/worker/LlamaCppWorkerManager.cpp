// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifdef QAI_FORGE_BUILD_LLAMACPP

#include "qai_forge/worker/LlamaCppWorkerManager.h"

LlamaCppWorkerManager::LlamaCppWorkerManager()
    : InferenceWorkerManager("llamacpp") {
}

#endif // QAI_FORGE_BUILD_LLAMACPP
