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

// Handle to the LLMObject C++
typedef void* LLMHandle;

// Callback for Token
typedef void (*LLMResponseCallback)(const Response* response);

//invokes the Constructor of LLM Object
LLMHandle llm_create_object(
    const char* model,
    char* config_path,
    bool streaming);

void llm_reset_object(LLMHandle handle); //Reset the Dialog of LLM Object

//invokes the Destructor of LLM Object
void llm_destroy_object(LLMHandle handle);

// Chat completion function
void llm_chat_completion_create(
    LLMHandle handle,
    const Query* query,
    bool streaming,
    LLMResponseCallback cb);

// Opaque handle to the C++ VLMObject
typedef void* VLMHandle;

/**
 * @brief Create a VLMObject.
 *
 * @param model    Model identifier string (e.g., "QWEN2_5_VL_3B").
 * @param config_path Path to the model configuration file.
 * @param streaming Enable streaming mode (true) or non‑streaming (false).
 * @return Opaque handle to the created VLMObject.
 */
VLMHandle vlm_create_object(const char* model, const char* config_path, bool streaming);

/**
 * @brief Destroy a VLMObject.
 *
 * @param handle Opaque handle returned by vlm_create_object.
 */
void vlm_destroy_object(VLMHandle handle);

/**
 * @brief Perform a VLM completion request.
 *
 * @param handle   VLMObject handle.
 * @param query    Pointer to a fully populated Query structure.
 * @param streaming Whether to stream partial results (must match the mode used at creation).
 * @param cb       Callback invoked when a response (or partial token) is ready.
 */
void vlm_chat_completion_create(VLMHandle handle,
                                const Query* query,
                                bool streaming,
                                LLMResponseCallback cb);
