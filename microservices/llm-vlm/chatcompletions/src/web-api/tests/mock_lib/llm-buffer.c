//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

//#include <bsd/string.h>  // Added for strlcpy
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "llm-buffer.h"

LLMHandle llm_create_object(const char* model, bool streaming) {
    return malloc(1);
}

void llm_destroy_object(LLMHandle handle) {
    free(handle);
}

void llm_chat_completion_retrieve(LLMHandle handle) {
    if (handle) {
    }
}

void llm_chat_completion_delete(LLMHandle handle) {
    if (handle) {
    }
}

void llm_chat_completion_create(LLMHandle handle, const Query* query, bool streaming, LLMResponseCallback callback) {
    printf("\nMsg: %s: %s\n", query->message.role, query->message.content);
    fflush(stdout);

    if (!handle) {
        printf("\nInvalid handle..\n");
        fflush(stdout);
        return;
    }

    Response rspns;
    memset(&rspns, 0, sizeof(Response));

    // Fill response metadata
    strlcpy(rspns.id, "chat-123", sizeof(rspns.id));
    strlcpy(rspns.object, "chat.completion", sizeof(rspns.object));
    rspns.created = 12345.0f;
    strlcpy(rspns.model, "genie-llm", sizeof(rspns.model));

    // Fill choice metadata
    strlcpy(rspns.choices[0].message.role, "assistant", sizeof(rspns.choices[0].message.role));
    rspns.choices[0].index = 0;
    rspns.choices[0].logprobs = true;

    const char* full_response = "The capital of India is New Delhi";

    if (streaming) {
        char buffer[512];
        strlcpy(buffer, full_response, sizeof(buffer));

        char *saveptr;
        char *token = strtok_r(buffer, " ", &saveptr);

        while (token != NULL) {
            memset(rspns.choices[0].message.content, 0, MAX_CONTENT_LENGTH);
            strlcpy(rspns.choices[0].message.content, token, MAX_CONTENT_LENGTH);
            strlcpy(rspns.choices[0].finish_reason, "", sizeof(rspns.choices[0].finish_reason));
            callback(&rspns);
            usleep(200000);

            token = strtok_r(NULL, " ", &saveptr);
        }

        memset(rspns.choices[0].message.content, 0, MAX_CONTENT_LENGTH);
        strlcpy(rspns.choices[0].finish_reason, "stop", sizeof(rspns.choices[0].finish_reason));
        callback(&rspns);
    } else {
        strlcpy(rspns.choices[0].message.content, full_response, MAX_CONTENT_LENGTH);
        strlcpy(rspns.choices[0].finish_reason, "stop", sizeof(rspns.choices[0].finish_reason));

        callback(&rspns);
    }

    printf("\nCompleted response dispatch.\n");
    fflush(stdout);
}
