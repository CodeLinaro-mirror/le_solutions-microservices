//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#pragma once

#include "llm-buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to the C++ LLMObject
typedef void* LLMHandle;

LLMHandle llm_create_object(const char* model); //invokes the Constructor of LLM Object
void llm_destroy_object(LLMHandle handle); //invokes the Destructor of LLM Object

// Chat completion functions
void llm_chat_completion_create(LLMHandle handle, const Query* query, Response* response);
void llm_chat_completion_retrieve(LLMHandle handle);
void llm_chat_completion_delete(LLMHandle handle);

#ifdef __cplusplus
}
#endif

