// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#ifdef QAI_FORGE_BUILD_LLAMACPP

#include "qai_forge/utils/GgufMetadataReader.h"
#include "qai_forge/utils/Logger.h"

// llama.cpp GGUF header API
#include <gguf.h>

#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// readMetadata — Parse GGUF file header without loading weights
// ─────────────────────────────────────────────────────────────────────────────
GgufMetadataReader::GgufMetadata GgufMetadataReader::readMetadata(const std::string& ggufPath) {
    GgufMetadata result;

    // Initialize GGUF context with no_alloc=true to avoid loading tensor weights
    struct gguf_init_params params;
    params.no_alloc = true;
    params.ctx = nullptr;

    struct gguf_context* ctx = gguf_init_from_file(ggufPath.c_str(), params);
    if (!ctx) {
        LOG_WARN("[GgufMetadataReader] Failed to open GGUF file: " << ggufPath);
        return result;  // isValid=false
    }

    // Extract metadata from GGUF header
    // These keys are standard across GGUF files from llama.cpp ecosystem
    result.chatTemplate = extractString(ctx, "tokenizer.chat_template");
    result.contextLength = static_cast<int32_t>(extractUint32(ctx, "llama.context_length", 4096));
    result.architecture = extractString(ctx, "general.architecture");

    // Mark as valid if we successfully opened the file
    result.isValid = true;

    // Free the GGUF context (releases header memory, no weights were loaded)
    gguf_free(ctx);

    LOG_DEBUG("[GgufMetadataReader] Parsed " << ggufPath
              << ": arch=" << (result.architecture.empty() ? "(unknown)" : result.architecture)
              << " ctx=" << result.contextLength
              << " template=" << (result.chatTemplate.empty() ? "(none)" : "present"));

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// extractString — Get string value from GGUF context by key
// ─────────────────────────────────────────────────────────────────────────────
std::string GgufMetadataReader::extractString(struct gguf_context* ctx, const char* key) {
    int key_id = gguf_find_key(ctx, key);
    if (key_id == -1) {
        return "";  // Key not found
    }

    // Check if the value is actually a string type
    enum gguf_type type = gguf_get_kv_type(ctx, key_id);
    if (type != GGUF_TYPE_STRING) {
        LOG_DEBUG("[GgufMetadataReader] Key '" << key << "' is not a string (type=" << static_cast<int>(type) << ")");
        return "";
    }

    const char* value = gguf_get_val_str(ctx, key_id);
    return value ? std::string(value) : "";
}

// ─────────────────────────────────────────────────────────────────────────────
// extractInt32 — Get int32 value from GGUF context by key
// ─────────────────────────────────────────────────────────────────────────────
int32_t GgufMetadataReader::extractInt32(struct gguf_context* ctx, const char* key, int32_t defaultValue) {
    int key_id = gguf_find_key(ctx, key);
    if (key_id == -1) {
        return defaultValue;  // Key not found
    }

    enum gguf_type type = gguf_get_kv_type(ctx, key_id);
    if (type != GGUF_TYPE_INT32) {
        LOG_DEBUG("[GgufMetadataReader] Key '" << key << "' is not int32 (type=" << static_cast<int>(type) << ")");
        return defaultValue;
    }

    return gguf_get_val_i32(ctx, key_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// extractUint32 — Get uint32 value from GGUF context by key
// ─────────────────────────────────────────────────────────────────────────────
uint32_t GgufMetadataReader::extractUint32(struct gguf_context* ctx, const char* key, uint32_t defaultValue) {
    int key_id = gguf_find_key(ctx, key);
    if (key_id == -1) {
        return defaultValue;  // Key not found
    }

    enum gguf_type type = gguf_get_kv_type(ctx, key_id);
    if (type != GGUF_TYPE_UINT32) {
        // Try int32 as fallback (some GGUF files may use signed int for context length)
        if (type == GGUF_TYPE_INT32) {
            int32_t val = gguf_get_val_i32(ctx, key_id);
            return val > 0 ? static_cast<uint32_t>(val) : defaultValue;
        }
        LOG_DEBUG("[GgufMetadataReader] Key '" << key << "' is not uint32 (type=" << static_cast<int>(type) << ")");
        return defaultValue;
    }

    return gguf_get_val_u32(ctx, key_id);
}

#endif // QAI_FORGE_BUILD_LLAMACPP
