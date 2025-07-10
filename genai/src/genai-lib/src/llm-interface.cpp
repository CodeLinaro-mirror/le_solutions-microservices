//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================
#include "llm-interface.h"
#include "llm-service.hpp"

extern "C" {

LLMHandle llm_create_object(const char* model) { //Creates LLM Object
    return new LLMObject(std::string(model));
}

void llm_destroy_object(LLMHandle handle) { //Destroys Handle
    delete static_cast<LLMObject*>(handle);
}

void llm_chat_completion_create(LLMHandle handle, const Query* query, Response* response) { //Calls Chat Completion Create
    if (handle) {
        LLMObject* obj = static_cast<LLMObject*>(handle);
        //First populate Query struct in LLM Object with the input struct from REST API side
        *(obj->query) = *query;
        // Call chat completion create to run query
        obj->chat_completion_create();
        *response = *(obj->response);
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
