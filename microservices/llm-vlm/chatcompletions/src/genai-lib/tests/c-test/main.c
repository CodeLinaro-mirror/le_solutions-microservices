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


void my_token_callback(const TokenResponse* token) {
    // Print the token content if not empty
    if (strlen(token->content) > 0) {
        printf("%s", token->content);
        fflush(stdout);  // Ensure immediate output
    }

    // Check if generation is complete
    if (strcmp(token->finish_reason, "stop") == 0) {
        printf("\n");  // Add newline at end
    }
}

int main() {

    char model[16];
    char config_path[256];

    printf("What model will be used? 0 for LLAMA3_1_8B, 1 for LLAMA3_2_3B, and 2 for QWEN2_5_7B: ");
    char choiceString[16];

    if (!fgets(choiceString, sizeof(choiceString), stdin)) {
        fprintf(stderr, "Error reading input.\n");
    }

    int choice = atoi(choiceString);

    switch (choice) {
        case 0:
            strlcpy(model, "LLAMA3_1_8B", sizeof(model));
            strlcpy(config_path, "genie_config_llama3_1_8B.json", sizeof(config_path));
            break;

        case 1:
            strlcpy(model, "LLAMA3_2_3B", sizeof(model));
            strlcpy(config_path, "genie_config_llama3_2_3B.json", sizeof(config_path));
            break;

        case 2:
            strlcpy(model, "QWEN2_5_7B", sizeof(model));
            strlcpy(config_path, "genie_config_qwen2_5_7B.json", sizeof(config_path));
            break;

        default:
            printf("ERROR Unsupported model selected");
            break;
    }

    printf("Do you want to enable streaming? 0 for No Stream and 1 for Yes Stream: ");

    char choiceStringStream[16];

    if (!fgets(choiceStringStream, sizeof(choiceStringStream), stdin)) {
        fprintf(stderr, "Error reading input.\n");
    }

    int choiceStream = atoi(choiceStringStream);
    bool stream = false;

    switch (choiceStream) {
        case 0:
            stream = false;
            break;

        case 1:
            stream = true;
            break;

        default:
            printf("ERROR Unsupported option selected");
            break;
    }

    LLMHandle llm = llm_create_object(model, config_path, "sampler.json", stream);

    while (1) {
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

        llm_chat_completion_create(llm, &query, stream, my_token_callback);
    }

    llm_destroy_object(llm);
    return 1; //Success
}
