// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#ifdef QAI_FORGE_BUILD_LLAMACPP

#include <string>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// GgufMetadataReader — Zero-copy GGUF file header parser
//
// Reads metadata from GGUF model files without loading the multi-gigabyte
// tensor weights into memory. Uses the gguf.h C API from llama.cpp with
// {.no_alloc = true} to parse only the 1-2KB file header.
//
// Usage:
//   auto meta = GgufMetadataReader::readMetadata("/models/model.gguf");
//   if (meta.isValid) {
//       std::cout << "Context: " << meta.contextLength << "\n";
//       std::cout << "Template: " << meta.chatTemplate << "\n";
//   }
//
// Extracted metadata:
//   - tokenizer.chat_template  → Jinja2 template string
//   - llama.context_length     → Max context window (tokens)
//   - general.architecture     → Model architecture (llama, qwen2, etc.)
//
// Graceful fallback:
//   - Missing keys → defaults (context=4096, empty template)
//   - Invalid file → isValid=false
//   - Always calls gguf_free() to release header memory
//
// This header is only included when QAI_FORGE_BUILD_LLAMACPP is defined.
// When the llama.cpp backend is disabled, this file is not compiled.
// ─────────────────────────────────────────────────────────────────────────────

class GgufMetadataReader {
public:
    struct GgufMetadata {
        std::string chatTemplate;      // Jinja2 chat template from tokenizer.chat_template
        int32_t contextLength;          // Max context window from llama.context_length
        std::string architecture;       // Model architecture from general.architecture
        bool isValid;                   // True if file was successfully parsed

        GgufMetadata()
            : chatTemplate("")
            , contextLength(4096)
            , architecture("")
            , isValid(false)
        {}
    };

    /**
     * Read metadata from a GGUF file without loading model weights.
     *
     * @param ggufPath Absolute path to the .gguf file
     * @return GgufMetadata struct with extracted values (isValid=false on error)
     */
    static GgufMetadata readMetadata(const std::string& ggufPath);

private:
    // Pure static utility class — no instances
    GgufMetadataReader() = delete;
    ~GgufMetadataReader() = delete;
    GgufMetadataReader(const GgufMetadataReader&) = delete;
    GgufMetadataReader& operator=(const GgufMetadataReader&) = delete;

    /**
     * Extract a string value from GGUF context by key.
     * Returns empty string if key not found or wrong type.
     */
    static std::string extractString(struct gguf_context* ctx, const char* key);

    /**
     * Extract an int32 value from GGUF context by key.
     * Returns defaultValue if key not found or wrong type.
     */
    static int32_t extractInt32(struct gguf_context* ctx, const char* key, int32_t defaultValue);

    /**
     * Extract a uint32 value from GGUF context by key.
     * Returns defaultValue if key not found or wrong type.
     */
    static uint32_t extractUint32(struct gguf_context* ctx, const char* key, uint32_t defaultValue);
};

#endif // QAI_FORGE_BUILD_LLAMACPP
