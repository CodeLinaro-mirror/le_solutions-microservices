//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <llm-interface.h>

int main() {

    char model[16];

    printf("What model will be used? 0 for LLAMA3_1_8B, 1 for LLAMA3_2_3B, and 2 for QWEN2_5_7B: ");
    char choiceString[16];

    if (!fgets(choiceString, sizeof(choiceString), stdin)) {
        fprintf(stderr, "Error reading input.\n");
    }

    int choice = atoi(choiceString);

    switch(choice) {
        case 0:
            strlcpy(model, "LLAMA3_1_8B", sizeof(model));
            break;

        case 1:
            strlcpy(model, "LLAMA3_2_3B", sizeof(model));
            break;

        case 2:
            strlcpy(model, "QWEN2_5_7B", sizeof(model));
            break;

        default:
            printf("ERROR Unsupported model selected");
            break;
    }

    LLMHandle llm = llm_create_object(model);

    while(1) {

        Message message;

        char userQuery[1024];
        printf("Enter your prompt: ");
        if (!fgets(userQuery, sizeof(userQuery), stdin)) {
            fprintf(stderr, "Error reading prompt.\n");
            break;
        }

        strlcpy(message.content, userQuery, sizeof(message.content));

        strlcpy(message.role, "user", sizeof(message.role));

        Query query;
        query.message = message;
        strlcpy(query.model, model, sizeof(query.model));

        Response response;

        llm_chat_completion_create(llm, &query, &response);

        printf("%s\n", response.choices[0].message.content);

    }

    llm_destroy_object(llm);
    return 1; //Success
}
