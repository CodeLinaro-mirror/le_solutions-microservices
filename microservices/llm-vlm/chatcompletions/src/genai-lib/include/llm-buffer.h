//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#ifndef LLM_BUFFER_H
#define LLM_BUFFER_H

#include <stdbool.h>
#include <stddef.h>

#define MAX_STRING_LENGTH 256
#define MAX_CONTENT_LENGTH 12300
#define MAX_MESSAGES 100
#define MAX_CHOICES 10
#define MAX_CONTENT_ITEMS 20  // Maximum number of content items per message

/*--------------------------------------------------------------------
 * Content item types for OpenAI‑compatible messages
 *--------------------------------------------------------------------*/
typedef enum {
    CONTENT_TYPE_TEXT = 0,          // Simple text string
    CONTENT_TYPE_IMAGE_BUFFER = 1   // Direct image buffer with size
} ContentType;

/* Image buffer representation – direct memory buffer */
typedef struct {
    void* buffer;      // Pointer to image data
    size_t size;       // Size of the buffer in bytes
} Image;

/* A single content item – either text or image buffer */
typedef struct {
    ContentType type;
    union {
        char text[MAX_CONTENT_LENGTH];
        Image image;           // Direct image buffer
    };
} ContentItem;

/*--------------------------------------------------------------------
 * Message structure – supports both LLM (text only) and VLM (multimodal)
 *--------------------------------------------------------------------*/
typedef struct {
    char role[MAX_STRING_LENGTH];          // "user", "assistant", etc.

    /* Backward‑compatible single‑string content (used by pure LLM) */
    char content[MAX_CONTENT_LENGTH];

    /* Multimodal content – array of items (text + image files) */
    ContentItem content_items[MAX_CONTENT_ITEMS];
    int content_items_count;               // Number of valid items in the array

    /* Flag indicating which representation is active:
     *   false – use the simple `content` string (LLM mode)
     *   true  – use `content_items` array (VLM mode)                */
    bool use_content_items;
} Message;

typedef struct {
    int completion_tokens;
    int prompt_tokens;
    int total_tokens;
} Usage;

typedef struct {
    Message message;
    int index;
    bool logprobs;
    char finish_reason[MAX_STRING_LENGTH];
} Choices;

typedef struct {
    Message message;
    char model[MAX_STRING_LENGTH];
    int max_completion_tokens;
    float temperature;
    float top_p;
    int top_k;
    float seed;
    float presence_penalty;
    float frequency_penalty;
} Query;

typedef struct {
    char id[MAX_STRING_LENGTH];
    char object[MAX_STRING_LENGTH];
    float created;
    char model[MAX_STRING_LENGTH];
    Choices choices[MAX_CHOICES];
    Usage usage;
} Response;

// Lightweight structure for per-token streaming callbacks
typedef struct {
    char id[MAX_STRING_LENGTH];              //request ID
    char model[MAX_STRING_LENGTH];           //model name
    char content[MAX_STRING_LENGTH];         //token content
    char finish_reason[MAX_STRING_LENGTH];   //"stop", "length", etc.
} TokenResponse;

/* Callback for individual tokens (streaming) */
typedef void (*LLMTokenCallback)(const TokenResponse* token);

#endif  // LLM_BUFFER_H
