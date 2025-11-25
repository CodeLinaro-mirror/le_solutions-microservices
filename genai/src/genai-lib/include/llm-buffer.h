//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

#ifndef LLM_BUFFER_H
#define LLM_BUFFER_H

#include <stdbool.h>

#define MAX_STRING_LENGTH 256
#define MAX_CONTENT_LENGTH 12300
#define MAX_MESSAGES 100
#define MAX_CHOICES 10

typedef struct {
    int completion_tokens;
    int prompt_tokens;
    int total_tokens;
} Usage;

typedef struct {
    char role[MAX_STRING_LENGTH];
    char content[MAX_CONTENT_LENGTH];
} Message;

typedef struct {
    Message message;

    int index;
    bool logprobs;
    char finish_reason[MAX_STRING_LENGTH];
} Choices;

typedef struct {
    // Supported by Genie
    Message message;
    char model[MAX_STRING_LENGTH];
    int max_completion_tokens;
    float temperature;
    float top_p;
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

// Callback for Token
typedef void (*LLMResponseCallback)(const Response* response);

#endif  // LLM_BUFFER_H
