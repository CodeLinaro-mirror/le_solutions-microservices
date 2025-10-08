//=============================================================================
//
// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear
//
//=============================================================================

typedef _Bool bool;

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

    // Not supported by Genie
    int index;
    bool logprobs;
    char finish_reason[MAX_STRING_LENGTH];
} Choices;

typedef struct {
    // Not Supported by Genie
    //int frequency_penalty;
    //bool logprobs;
    //int n;
    //bool parallel_tool_calls;
    //int presence_penalty;
    //bool stream;
    //int top_logprobs;

    // Supported by Genie
    Message message;
    char model[MAX_STRING_LENGTH];
    //int max_completion_tokens;
    //float temperature;
    //float top_k;
    //float top_p;
} Query;

typedef struct {
    char id[MAX_STRING_LENGTH];
    char object[MAX_STRING_LENGTH];
    float created;
    char model[MAX_STRING_LENGTH];
    Choices choices[MAX_CHOICES];
    Usage usage;
} Response;


// Handle to the LLMObject C++
typedef void* LLMHandle;

LLMHandle llm_create_object(const char* model); //invokes the Constructor of LLM Object
void llm_destroy_object(LLMHandle handle); //invokes the Destructor of LLM Object

// Chat completion functions
void llm_chat_completion_create(LLMHandle handle, const Query* query, Response* response);
void llm_chat_completion_retrieve(LLMHandle handle);
void llm_chat_completion_delete(LLMHandle handle);