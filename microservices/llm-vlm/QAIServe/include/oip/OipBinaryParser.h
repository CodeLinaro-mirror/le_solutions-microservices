// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// OipBinaryParser — Parse OIP binary extension requests
//
// The OIP binary extension allows tensor data to be sent as raw bytes
// instead of JSON arrays, avoiding the overhead of number serialization.
//
// Wire format:
//   Content-Type: application/octet-stream
//   Inference-Header-Content-Length: <N>
//   Body: <N bytes of JSON header> <raw binary tensor data>
//
// The JSON header uses "binary_data_size" in the tensor parameters to indicate
// how many bytes of binary data follow for that tensor:
//
//   {"inputs":[
//     {"name":"image","shape":[1,224,224,3],"datatype":"FP32",
//      "parameters":{"binary_data_size":602112}}
//   ]}
//   <602112 bytes of raw float32 little-endian>
//
// Multiple tensors: binary data is concatenated in input order.
// ─────────────────────────────────────────────────────────────────────────────

#include "oip/OipDTOs.h"
#include <string>
#include <stdexcept>

struct OipBinaryParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class OipBinaryParser {
public:
    /**
     * Parse an OIP binary extension request body.
     *
     * @param body        Raw HTTP request body bytes
     * @param header_len  Value of Inference-Header-Content-Length header
     * @return            Parsed OipInferRequest with tensor data populated
     * @throws            OipBinaryParseError on malformed input
     */
    static OipInferRequest parse(const std::string& body, size_t header_len);

    /**
     * Parse a standard JSON /infer request (no binary extension).
     *
     * @param body  JSON request body
     * @return      Parsed OipInferRequest
     * @throws      OipBinaryParseError on malformed JSON
     */
    static OipInferRequest parseJson(const std::string& body);

    /**
     * Detect whether a request uses the binary extension.
     * Returns true if the Inference-Header-Content-Length header is present.
     */
    static bool isBinaryRequest(const std::string& header_value);

private:
    static OipTensorInput parseTensorJson(const nlohmann::json& j);
    static size_t datatypeBytes(const std::string& datatype);
};
