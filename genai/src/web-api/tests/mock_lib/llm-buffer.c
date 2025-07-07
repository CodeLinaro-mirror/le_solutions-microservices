//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "llm-buffer.h"



LLMHandle llm_create_object(const char *model) { //Creates LLM Object
    return malloc(1);
}

void llm_destroy_object(LLMHandle handle) { //Destroys Handle
    free(handle);
}

void llm_chat_completion_create(LLMHandle handle, const Query* query, Response* response) { //Calls Chat Completion Create
    printf("\n Msg : %s: %s", query->message.role, query->message.content);
    fflush(stdout);
    if (handle) {
        strcpy(response->id, "chat-123");

        strcpy(response->choices[0].message.role, "assistant");

        strcpy(response->choices[0].message.content, "The capital of india is new delhi");

        strcpy(response->choices[0].finish_reason,"stop");

        response->choices[0].logprobs = true;
        response->choices[0].index = 0;

        strcpy(response->object, "chat.completion");

        response->created = 12345.0f;
        printf("\n Valid handel..\n");
        fflush(stdout);
    }
    else
    {
        printf("\n Invalid handel..\n");
        fflush(stdout);
    }
}

void llm_chat_completion_retrieve(LLMHandle handle) { //Calls Chat Completion Retrieve
    if (handle) {
        
    }
}

void llm_chat_completion_delete(LLMHandle handle) { //Calls Chat Completion Delete
    if (handle) {
        
    }
}


