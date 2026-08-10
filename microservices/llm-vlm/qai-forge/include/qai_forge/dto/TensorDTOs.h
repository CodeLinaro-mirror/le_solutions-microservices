// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <string>
#include <vector>
#include <cctype>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// TensorDTOs — Transport-agnostic DTOs for Predictive AI inference
//
// These DTOs sit at the Layer 1 / Layer 2 boundary for Predictive AI models
// (classification, detection, segmentation). They mirror the KFServing v2
// inference protocol wire format but contain raw bytes — no base64, no HTTP
// multipart. The transport layer (Layer 1) handles encoding/decoding.
//
// Design invariants:
//   - Transport layer decodes base64/multipart → InputTensor.data (raw bytes)
//   - Orchestration layer receives raw bytes, runs inference, returns raw bytes
//   - Transport layer encodes OutputTensor.data → base64/JSON arrays
//   - The orchestration layer NEVER sees base64 or HTTP multipart
//
// See docs/unified-inference-service.md §4 for the full design rationale.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * Tensor element data type.
 * Matches KFServing v2 datatype strings.
 */
enum class TensorDataType {
    FLOAT32,   // "FP32"
    FLOAT16,   // "FP16"
    INT32,     // "INT32"
    INT16,     // "INT16"
    INT8,      // "INT8"
    UINT8,     // "UINT8"
    BOOL,      // "BOOL"
    BYTES,     // "BYTES" — raw byte buffer
};

/**
 * Convert TensorDataType to KFServing v2 datatype string.
 */
inline std::string tensorDataTypeToString(TensorDataType dt) {
    switch (dt) {
        case TensorDataType::FLOAT32: return "FP32";
        case TensorDataType::FLOAT16: return "FP16";
        case TensorDataType::INT32:   return "INT32";
        case TensorDataType::INT16:   return "INT16";
        case TensorDataType::INT8:    return "INT8";
        case TensorDataType::UINT8:   return "UINT8";
        case TensorDataType::BOOL:    return "BOOL";
        case TensorDataType::BYTES:   return "BYTES";
        default:                      return "FP32";
    }
}

/**
 * Parse KFServing v2 datatype string to TensorDataType.
 *
 * Case-insensitive, and also accepts each backend engine's native spelling
 * (e.g. SNPEEngine's "float32"/"uint8" from snpeEncodingToString(), not just
 * the KFServing v2 "FP32"/"UINT8" abbreviations) — without this, any
 * non-canonical spelling silently fell through to the FLOAT32 default below,
 * mislabeling every quantized SNPE-backend output tensor as 4-byte float and
 * corrupting downstream byte-width math (e.g. PostprocessUtils::readFloat).
 */
inline TensorDataType tensorDataTypeFromString(const std::string& s) {
    std::string upper;
    upper.reserve(s.size());
    for (char c : s) upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    if (upper == "FP32"  || upper == "FLOAT32" || upper == "FLOAT")  return TensorDataType::FLOAT32;
    if (upper == "FP16"  || upper == "FLOAT16")                      return TensorDataType::FLOAT16;
    if (upper == "INT32" || upper == "UINT32")                       return TensorDataType::INT32;
    if (upper == "INT16" || upper == "UINT16")                       return TensorDataType::INT16;
    if (upper == "INT8")                                             return TensorDataType::INT8;
    if (upper == "UINT8")                                            return TensorDataType::UINT8;
    if (upper == "BOOL")                                             return TensorDataType::BOOL;
    if (upper == "BYTES")                                            return TensorDataType::BYTES;
    return TensorDataType::FLOAT32;  // safe default
}

/**
 * A single input tensor.
 * data contains raw bytes — the transport layer decoded base64/JSON arrays
 * before constructing this struct.
 */
struct InputTensor {
    std::string           name;   // tensor name (e.g. "input_0")
    std::vector<int64_t>  shape;  // tensor shape (e.g. [1, 224, 224, 3])
    TensorDataType        dtype;  // element data type
    std::vector<uint8_t>  data;   // raw bytes (transport-decoded, never base64)
};

/**
 * A single output tensor.
 * data contains raw bytes — the transport layer encodes to base64/JSON arrays
 * before sending to the client.
 */
struct OutputTensor {
    std::string           name;   // tensor name (e.g. "output_0")
    std::vector<int64_t>  shape;  // tensor shape (e.g. [1, 1000])
    TensorDataType        dtype;  // element data type
    std::vector<uint8_t>  data;   // raw bytes (transport encodes to base64/JSON)
};

/**
 * Inference performance statistics.
 * Populated by the backend after inference completes.
 */
struct InferenceStats {
    double      latency_ms   = 0.0;  // end-to-end inference latency
    std::string backend_name;        // "LiteRT", "QNN", etc.
};

/**
 * Predictive AI inference request.
 * Parsed from HTTP/gRPC/D-Bus by Layer 1, passed to Layer 2.
 * Layer 2 NEVER touches HTTP, gRPC, or D-Bus.
 */
struct TensorInferenceRequest {
    std::string              model;        // model identifier
    std::string              request_id;   // unique request ID (generated by Layer 1)
    std::vector<InputTensor> inputs;       // input tensors (raw bytes)
    std::vector<std::string> output_names; // which outputs to return (empty = all)
};

/**
 * Predictive AI inference response.
 * Returned by Layer 2 to Layer 1.
 * Layer 1 formats this into the KFServing v2 JSON wire format.
 */
struct TensorInferenceResponse {
    std::string               model;      // model identifier
    std::string               request_id; // matches TensorInferenceRequest.request_id
    std::vector<OutputTensor> outputs;    // output tensors (raw bytes)
    InferenceStats            stats;      // latency, backend name
};
