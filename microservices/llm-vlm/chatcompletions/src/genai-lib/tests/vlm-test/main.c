//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "vlm-interface.h"

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
    char model[128];
    char config_path[512];
    char choiceStr[8];

    // Model input
    printf("Enter model name (e.g. qwen2.5_vl_7b_instruct or leave empty for default QWEN2_5_VL_3B): ");
    if (!fgets(model, sizeof(model), stdin)) {
        fprintf(stderr, "Failed to read input.\n");
        return 1;
    }
    model[strcspn(model, "\r\n")] = '\0'; // Trim newline
    if (strlen(model) == 0) {
        strlcpy(model, "QWEN2_5_VL_3B", sizeof(model));
    }

    // Config path input
    printf("Enter config json path (or leave empty for vlm.json): ");
    if (!fgets(config_path, sizeof(config_path), stdin)) {
        fprintf(stderr, "Failed to read input.\n");
        return 1;
    }
    config_path[strcspn(config_path, "\r\n")] = '\0'; // Trim newline
    if (strlen(config_path) == 0) {
        strlcpy(config_path, "vlm.json", sizeof(config_path));
    }

    // Sampler path input
    char sampler_path[512];
    printf("Enter sampler json path (or leave empty for sampler.json): ");
    if (!fgets(sampler_path, sizeof(sampler_path), stdin)) {
        fprintf(stderr, "Failed to read input.\n");
        return 1;
    }
    sampler_path[strcspn(sampler_path, "\r\n")] = '\0'; // Trim newline
    if (strlen(sampler_path) == 0) {
        strlcpy(sampler_path, "sampler.json", sizeof(sampler_path));
    }

    printf("Enable streaming? (0 = no, 1 = yes): ");
    if (!fgets(choiceStr, sizeof(choiceStr), stdin)) {
        fprintf(stderr, "Failed to read input.\n");
        return 1;
    }
    bool stream = atoi(choiceStr) == 1;

    VLMHandle vlm = vlm_create_object(model, config_path, sampler_path, stream);
    if (!vlm) {
        fprintf(stderr, "Failed to create VLM object.\n");
        return 1;
    }

    while (1) {
        Message message = {0};
        void* imageBuffer = NULL;

        // Prompt for user text
        char userText[1024];
        printf("Enter your prompt (or empty to exit): ");
        if (!fgets(userText, sizeof(userText), stdin)) {
            fprintf(stderr, "Failed to read prompt.\n");
            break;
        }
        userText[strcspn(userText, "\r\n")] = '\0'; // Trim newline
        if (strlen(userText) == 0) {
            break;
        }

        // Prompt for optional image file path
        char imagePath[512];
        printf("Enter image file path (or leave empty for no image): ");
        if (!fgets(imagePath, sizeof(imagePath), stdin)) {
            fprintf(stderr, "Failed to read image path.\n");
            break;
        }
        imagePath[strcspn(imagePath, "\r\n")] = '\0'; // Trim newline

        // Automatically inject vision tokens into the prompt if an image is provided
        // but the user didn't manually type the vision tokens.
        char finalPrompt[2048];
        if (strlen(imagePath) > 0 && !strstr(userText, "<|vision_start|>")) {
            snprintf(finalPrompt, sizeof(finalPrompt), "<|vision_start|><|vision_end|> %s", userText);
        } else {
            strlcpy(finalPrompt, userText, sizeof(finalPrompt));
        }

        // Fill multimodal content items
        message.use_content_items = true;
        message.content_items_count = 0;

        // Text item
        message.content_items[message.content_items_count].type = CONTENT_TYPE_TEXT;
        strlcpy(message.content_items[message.content_items_count].text,
                finalPrompt,
                sizeof(message.content_items[message.content_items_count].text));
        message.content_items_count++;

        // Image buffer if provided
        size_t imageSize = 0;
        if (strlen(imagePath) > 0) {
            // Read the image file into a buffer
            FILE* imageFile = fopen(imagePath, "rb");
            if (!imageFile) {
                fprintf(stderr, "Failed to open image file: %s\n", imagePath);
                continue;
            }

            // Get file size
            fseek(imageFile, 0, SEEK_END);
            imageSize = ftell(imageFile);
            fseek(imageFile, 0, SEEK_SET);

            // Allocate buffer
            imageBuffer = malloc(imageSize);
            if (!imageBuffer) {
                fprintf(stderr, "Failed to allocate memory for image buffer.\n");
                fclose(imageFile);
                continue;
            }

            // Read file into buffer
            size_t bytesRead = fread(imageBuffer, 1, imageSize, imageFile);
            fclose(imageFile);

            if (bytesRead != imageSize) {
                fprintf(stderr, "Failed to read complete image file.\n");
                free(imageBuffer);
                continue;
            }

            printf("Image loaded: %zu bytes\n", imageSize);

            // Add image buffer to content items
            message.content_items[message.content_items_count].type = CONTENT_TYPE_IMAGE_BUFFER;
            message.content_items[message.content_items_count].image.buffer = imageBuffer;
            message.content_items[message.content_items_count].image.size = imageSize;
            message.content_items_count++;
        }

        strlcpy(message.role, "user", sizeof(message.role));

        Query query = {0};
        query.message = message;
        query.temperature = 1.5;
        query.top_p = 0.5;
        strlcpy(query.model, model, sizeof(query.model));

        vlm_chat_completion_create(vlm, &query, stream, my_token_callback);

        // Free the image buffer after completion
        if (imageBuffer) {
            free(imageBuffer);
            imageBuffer = NULL;
        }
    }

    vlm_destroy_object(vlm);
    return 0;
}
