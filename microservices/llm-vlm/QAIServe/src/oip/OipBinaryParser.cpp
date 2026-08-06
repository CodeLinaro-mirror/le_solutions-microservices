// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "oip/OipBinaryParser.h"
#include <nlohmann/json.hpp>
#include <cstring>
#include <numeric>

using json = nlohmann::json;

// ─────────────────────────────────────────────────────────────────────────────
// datatypeBytes — bytes per element for a given OIP datatype string
// ─────────────────────────────────────────────────────────────────────────────
size_t OipBinaryParser::datatypeBytes(const std::string& datatype) {
    if (datatype == "FP32" || datatype == "INT32" || datatype == "UINT32") return 4;
    if (datatype == "FP16" || datatype == "INT16" || datatype == "UINT16") return 2;
    if (datatype == "INT8"  || datatype == "UINT8" || datatype == "BOOL")  return 1;
    if (datatype == "FP64" || datatype == "INT64" || datatype == "UINT64") return 8;
    return 4; // default FP32
}

// ─────────────────────────────────────────────────────────────────────────────
// parseTensorJson — parse a single input tensor from JSON
// ─────────────────────────────────────────────────────────────────────────────
OipTensorInput OipBinaryParser::parseTensorJson(const json& j) {
    OipTensorInput t;
    t.name     = j.value("name", "");
    t.datatype = j.value("datatype", "FP32");

    if (j.contains("shape") && j["shape"].is_array()) {
        for (const auto& d : j["shape"]) {
            t.shape.push_back(d.get<int64_t>());
        }
    }

    // Check for binary_data_size in parameters
    if (j.contains("parameters") && j["parameters"].is_object()) {
        const auto& params = j["parameters"];
        if (params.contains("binary_data_size")) {
            t.binary_data_size = params["binary_data_size"].get<int64_t>();
        }
    }

    // JSON data array (non-binary mode)
    if (t.binary_data_size == 0 && j.contains("data") && j["data"].is_array()) {
        size_t elem_bytes = datatypeBytes(t.datatype);
        for (const auto& val : j["data"]) {
            // Store as raw bytes (little-endian)
            if (t.datatype == "FP32") {
                float f = val.get<float>();
                uint8_t bytes[4];
                memcpy(bytes, &f, 4);
                t.data.insert(t.data.end(), bytes, bytes + 4);
            } else if (t.datatype == "INT32") {
                int32_t v = val.get<int32_t>();
                uint8_t bytes[4];
                memcpy(bytes, &v, 4);
                t.data.insert(t.data.end(), bytes, bytes + 4);
            } else if (t.datatype == "INT8" || t.datatype == "UINT8") {
                t.data.push_back(static_cast<uint8_t>(val.get<int>()));
            } else {
                // Generic: store as float32
                float f = val.get<float>();
                uint8_t bytes[4];
                memcpy(bytes, &f, 4);
                t.data.insert(t.data.end(), bytes, bytes + 4);
            }
            (void)elem_bytes;
        }
    }

    return t;
}

// ─────────────────────────────────────────────────────────────────────────────
// isBinaryRequest
// ─────────────────────────────────────────────────────────────────────────────
bool OipBinaryParser::isBinaryRequest(const std::string& header_value) {
    return !header_value.empty();
}

// ─────────────────────────────────────────────────────────────────────────────
// parseJson — standard JSON /infer request
// ─────────────────────────────────────────────────────────────────────────────
OipInferRequest OipBinaryParser::parseJson(const std::string& body) {
    OipInferRequest req;
    try {
        json j = json::parse(body);
        req.id = j.value("id", "");

        if (j.contains("inputs") && j["inputs"].is_array()) {
            for (const auto& inp : j["inputs"]) {
                req.inputs.push_back(parseTensorJson(inp));
            }
        }
        if (j.contains("outputs") && j["outputs"].is_array()) {
            for (const auto& out : j["outputs"]) {
                if (out.is_string()) {
                    req.outputs.push_back(out.get<std::string>());
                } else if (out.is_object() && out.contains("name")) {
                    req.outputs.push_back(out["name"].get<std::string>());
                }
            }
        }
    } catch (const json::exception& e) {
        throw OipBinaryParseError(std::string("JSON parse error: ") + e.what());
    }
    return req;
}

// ─────────────────────────────────────────────────────────────────────────────
// parse — OIP binary extension
//
// Body layout:
//   [0 .. header_len-1]  JSON header
//   [header_len .. end]  Binary tensor data (concatenated in input order)
// ─────────────────────────────────────────────────────────────────────────────
OipInferRequest OipBinaryParser::parse(const std::string& body, size_t header_len) {
    if (body.size() < header_len) {
        throw OipBinaryParseError(
            "Body too short: expected at least " + std::to_string(header_len)
            + " bytes for JSON header, got " + std::to_string(body.size()));
    }

    // Parse JSON header
    std::string json_str(body.data(), header_len);
    OipInferRequest req = parseJson(json_str);

    // Attach binary data to tensors that have binary_data_size set
    size_t binary_offset = header_len;
    for (auto& tensor : req.inputs) {
        if (tensor.binary_data_size <= 0) continue;

        size_t n = static_cast<size_t>(tensor.binary_data_size);
        if (binary_offset + n > body.size()) {
            throw OipBinaryParseError(
                "Binary data for tensor '" + tensor.name + "' extends beyond body: "
                "need " + std::to_string(n) + " bytes at offset "
                + std::to_string(binary_offset) + ", body size "
                + std::to_string(body.size()));
        }

        tensor.data.assign(
            reinterpret_cast<const uint8_t*>(body.data() + binary_offset),
            reinterpret_cast<const uint8_t*>(body.data() + binary_offset + n));
        binary_offset += n;
    }

    return req;
}
