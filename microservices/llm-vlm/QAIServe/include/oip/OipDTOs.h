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
//
// System shared memory extension (client + server on the same host):
//   /infer tensor inputs may set parameters.shared_memory_region (+ optional
//   shared_memory_offset) instead of "data"/binary_data_size; /infer
//   requested outputs may set the same parameters to have the server write
//   that output's bytes into the region instead of returning a "data"
//   array; /generate may set a top-level "images_shm" array instead of/
//   alongside "images". Regions are registered via
//   /v2/systemsharedmemory/region/{name}/register — see
//   shm/SharedMemoryManager.h.
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
    // Set when the client-facing system shared memory extension is used
    // (see shm/SharedMemoryManager.h) — data lives in a pre-registered POSIX
    // shm region instead of the HTTP body. Mutually exclusive with `data` /
    // binary_data_size; exactly one of the three must be set.
    std::string              shared_memory_region;   // empty = not using shm
    uint64_t                 shared_memory_offset = 0;
};

struct OipTensorOutput {
    std::string              name;
    std::vector<int64_t>     shape;
    std::string              datatype;
    std::vector<uint8_t>     data;       // raw bytes
};

// A requested output on /infer. `name` selects which model output to
// return; if `shared_memory_region` is set, the server memcpy's that
// output's bytes into the region (must be pre-registered, see
// shm/SharedMemoryManager.h) instead of returning them as a JSON "data"
// array — mirrors the input side's shared_memory_region on OipTensorInput,
// but in the opposite direction.
struct OipRequestedOutput {
    std::string               name;
    std::string               shared_memory_region;   // empty = not using shm
    uint64_t                  shared_memory_offset = 0;
};

struct OipInferRequest {
    std::string                        id;       // optional request ID
    std::vector<OipTensorInput>        inputs;
    std::vector<OipRequestedOutput>    outputs;  // requested outputs (empty = all, no shm)
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
    // Optional Genie memory scope for structured-message requests.
    std::string user;

    static OipGenerateParameters fromJson(const nlohmann::json& j);
};

// A client-facing system shared memory reference to one VLM image (see
// shm/SharedMemoryManager.h). `offset`/`byte_size` are relative to the named
// region, not the underlying shm segment — lets one region hold several
// images back to back.
struct OipShmImageRef {
    std::string region;
    uint64_t    offset    = 0;
    uint64_t    byte_size = 0;
};

struct OipGenerateRequest {
    // Either text_input (raw formatted prompt) or messages (server applies template)
    std::optional<std::string>              text_input;
    std::optional<nlohmann::json>           messages;   // OpenAI-style messages array

    // Raw image bytes for VLM models (from multipart upload or base64 in JSON)
    std::vector<std::vector<uint8_t>>       images;

    // Shared-memory image references (alternative to `images`) — resolved
    // by InferController and appended to `images` before inference.
    std::vector<OipShmImageRef>             image_shm_refs;

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
