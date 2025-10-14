//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================
#include "llm-interface.h"
#include "llm-service.hpp"

extern "C" {

LLMHandle llm_create_object(const char* model, bool streaming) { //Creates LLM Object
    return new LLMObject(std::string(model), streaming);
}

void llm_destroy_object(LLMHandle handle) { //Destroys Handle
    delete static_cast<LLMObject*>(handle);
}

void llm_chat_completion_create(LLMHandle handle, const Query* query, bool streaming,
                                LLMResponseCallback cb) { //Calls Chat Completion Create
   if (handle) {
        LLMObject* obj = static_cast<LLMObject*>(handle);
        // Set the Response callback
        obj->responseCallback = cb;
        //First populate Query struct in LLM Object with the input struct from REST API side
        *(obj->query) = *query;
        // Call chat completion create to run query
        obj->chat_completion_create();
    }
}

void llm_chat_completion_retrieve(LLMHandle handle) { //Calls Chat Completion Retrieve
    if (handle) {
        static_cast<LLMObject*>(handle)->chat_completion_retrieve();
    }
}

void llm_chat_completion_delete(LLMHandle handle) { //Calls Chat Completion Delete
    if (handle) {
        static_cast<LLMObject*>(handle)->chat_completion_delete();
    }
}

}
