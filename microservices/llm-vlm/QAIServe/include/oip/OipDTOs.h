// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// OipDTOs.h — Open Inference Protocol v2 Data Transfer Objects
//
// Covers:
//   - /v2/models/{model}/infer      (predictive — tensor I/O)
//   - /v2/models/{model}/generate   (generative — text I/O, blocking)
//   - /v2/models/{model}/generate_stream (generative — SSE streaming)
//
// Binary extension for /infer:
//   Content-Type: application/octet-stream
//   Inference-Header-Content-Length: <N>
//   Body: <N bytes JSON><raw tensor bytes>
//
// Multipart for /generate with images (VLM):
//   Content-Type: multipart/form-data
//   Parts: "request" (JSON) + "image_0", "image_1", ... (raw image bytes)
// ─────────────────────────────────────────────────────────────────────────────

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <nlohmann/json.hpp>

// ─────────────────────────────────────────────────────────────────────────────
// OIP /infer — tensor I/O (predictive models)
// ─────────────────────────────────────────────────────────────────────────────

struct OipTensorInput {
    std::string              name;
    std::vector<int64_t>     shape;
    std::string              datatype;   // "FP32", "INT32", "UINT8", etc.
    std::vector<uint8_t>     data;       // raw bytes (little-endian)
    // Set when binary extension is used (data is in the binary body section)
    int64_t                  binary_data_size = 0;
};

struct OipTensorOutput {
    std::string              name;
    std::vector<int64_t>     shape;
    std::string              datatype;
    std::vector<uint8_t>     data;       // raw bytes
};

struct OipInferRequest {
    std::string                      id;       // optional request ID
    std::vector<OipTensorInput>      inputs;
    std::vector<std::string>         outputs;  // requested output names (empty = all)
};

struct OipInferResponse {
    std::string                      model_name;
    std::string                      id;
    std::vector<OipTensorOutput>     outputs;

    nlohmann::json toJson() const;
};

// ─────────────────────────────────────────────────────────────────────────────
// OIP /generate — text I/O (generative models)
// ─────────────────────────────────────────────────────────────────────────────

struct OipGenerateParameters {
    int   max_new_tokens = 512;
    float temperature    = 1.0f;
    float top_p          = 1.0f;
    int   top_k          = 40;
    bool  stream         = false;
    std::string user;   // session ID for multi-turn (empty = stateless OIP mode)

    static OipGenerateParameters fromJson(const nlohmann::json& j);
};

struct OipGenerateRequest {
    // Either text_input (raw formatted prompt) or messages (server applies template)
    std::optional<std::string>              text_input;
    std::optional<nlohmann::json>           messages;   // OpenAI-style messages array

    // Raw image bytes for VLM models (from multipart upload or base64 in JSON)
    std::vector<std::vector<uint8_t>>       images;

    OipGenerateParameters                   parameters;

    static OipGenerateRequest fromJson(const nlohmann::json& j);
};

struct OipGenerateResponse {
    std::string model_name;
    std::string text_output;
    std::string reasoning_output;  // thinking models only (empty if not applicable)
    std::string finish_reason;     // "stop" | "length"
    int         prompt_tokens     = 0;
    int         completion_tokens = 0;

    nlohmann::json toJson() const;
};

// Single SSE chunk for /generate_stream
struct OipStreamChunk {
    std::string model_name;
    std::string text_output;        // token delta (intermediate chunks)
    std::string reasoning_output;   // thinking delta (thinking models only)
    std::string finish_reason;      // non-empty on final chunk
    int         completion_tokens  = 0;  // set on final chunk

    nlohmann::json toJson() const;
};
