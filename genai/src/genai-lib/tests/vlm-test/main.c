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

void my_response_callback(const Response* response) {
    // Print the assistant's response content
    printf("%s\n", response->choices[0].message.content);
}

int main() {
    char model[32];
    char config_path[256];
    strlcpy(config_path, "vlm.json", sizeof(config_path));
    printf("Select model (0: QWEN2_5_VL_3B): ");
    char choiceStr[8];
    if (!fgets(choiceStr, sizeof(choiceStr), stdin)) {
        fprintf(stderr, "Failed to read input.\n");
        return 1;
    }
    int choice = atoi(choiceStr);
    switch (choice) {
        case 0:
            strlcpy(model, "QWEN2_5_VL_3B", sizeof(model));
            break;
        default:
            printf("Unsupported model, defaulting to QWEN2_5_VL_3B.\n");
            strlcpy(model, "QWEN2_5_VL_3B", sizeof(model));
            break;
    }

    printf("Enable streaming? (0 = no, 1 = yes): ");
    if (!fgets(choiceStr, sizeof(choiceStr), stdin)) {
        fprintf(stderr, "Failed to read input.\n");
        return 1;
    }
    bool stream = atoi(choiceStr) == 1;

    VLMHandle vlm = vlm_create_object(model, config_path, stream);
    if (!vlm) {
        fprintf(stderr, "Failed to create VLM object.\n");
        return 1;
    }

    while (1) {
        Message message = {0};

        // Prompt for user text
        char userText[1024];
        printf("Enter your prompt (or empty to reuse same image): ");
        if (!fgets(userText, sizeof(userText), stdin)) {
            fprintf(stderr, "Failed to read prompt.\n");
            break;
        }
        if (strlen(userText) <= 1) { // only newline
            break;
        }

        // Prompt for optional image file path
        char imagePath[256];
        printf("Enter image file path (or leave empty for no image): ");
        if (!fgets(imagePath, sizeof(imagePath), stdin)) {
            fprintf(stderr, "Failed to read image path.\n");
            break;
        }
        // Trim newline characters
        imagePath[strcspn(imagePath, "\n")] = '\0';

        // Fill multimodal content items
        message.use_content_items = true;
        message.content_items_count = 0;

        // Text item
        message.content_items[message.content_items_count].type = CONTENT_TYPE_TEXT;
        strlcpy(message.content_items[message.content_items_count].text,
                userText,
                sizeof(message.content_items[message.content_items_count].text));
        message.content_items_count++;

        // Image buffer if provided
        void* imageBuffer = NULL;
        size_t imageSize = 0;
        if (strlen(imagePath) > 0) {
            // Read the image file into a buffer
            FILE* imageFile = fopen(imagePath, "rb");
            if (!imageFile) {
                fprintf(stderr, "Failed to open image file: %s\n", imagePath);
                break;
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
                break;
            }

            // Read file into buffer
            size_t bytesRead = fread(imageBuffer, 1, imageSize, imageFile);
            fclose(imageFile);

            if (bytesRead != imageSize) {
                fprintf(stderr, "Failed to read complete image file.\n");
                free(imageBuffer);
                break;
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
        vlm_chat_completion_create(vlm, &query, stream, my_response_callback);

        // Free the image buffer after completion
        if (imageBuffer) {
            free(imageBuffer);
            imageBuffer = NULL;
        }
    }

    vlm_destroy_object(vlm);
    return 0;
}
