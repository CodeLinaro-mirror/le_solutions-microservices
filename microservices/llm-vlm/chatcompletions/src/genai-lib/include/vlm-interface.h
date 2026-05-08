//=============================================================================
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//=============================================================================

#ifndef VLM_INTERFACE_H
#define VLM_INTERFACE_H

#include "llm-buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to the C++ VLMObject
typedef void* VLMHandle;

/**
 * @brief Create a VLMObject.
 *
 * @param model    Model identifier string (e.g., "QWEN2_5_VL_3B").
 * @param streaming Enable streaming mode (true) or non‑streaming (false).
 * @return Opaque handle to the created VLMObject.
 */
VLMHandle vlm_create_object(const char* model, const char* config_path, const char* sampler_config_path, bool streaming);

/**
 * @brief Destroy a VLMObject.
 *
 * @param handle Opaque handle returned by vlm_create_object.
 */
void vlm_destroy_object(VLMHandle handle);

/**
 * @brief Perform a VLM completion request.
 *
 * @param handle   VLMObject handle.
 * @param query    Pointer to a fully populated Query structure.
 * @param streaming Whether to stream partial results (must match the mode used at creation).
 * @param cb       Callback invoked when a response (or partial token) is ready.
 */
void vlm_chat_completion_create(VLMHandle handle,
                                const Query* query,
                                bool streaming,
                                LLMResponseCallback cb);

#ifdef __cplusplus
}
#endif

#endif // VLM_INTERFACE_H
