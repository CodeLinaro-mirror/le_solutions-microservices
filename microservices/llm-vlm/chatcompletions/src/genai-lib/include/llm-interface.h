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

//invokes the Constructor of LLM Object
LLMHandle llm_create_object(const char* model, const char* config_path,
        const char* sampler_config_path, bool streaming);

 //invokes the Destructor of LLM Object
void llm_destroy_object(LLMHandle handle);

//Reset the Dialog of LLM Object
void llm_reset_object(LLMHandle handle);

// Chat completion function with token callback
// token_cb receives TokenResponse for each token,
// with finish_reason set on final token
void llm_chat_completion_create(LLMHandle handle, const Query* query,
        bool streaming,
        LLMTokenCallback token_cb);

#ifdef __cplusplus
}
#endif
