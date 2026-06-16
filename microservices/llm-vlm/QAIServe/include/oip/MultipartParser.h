// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// MultipartParser — Parse multipart/form-data for OIP /generate (VLM)
//
// Parses a multipart/form-data body into named parts.
// Used by /v2/models/{model}/generate to accept raw image bytes alongside
// the JSON request, avoiding base64 encoding overhead.
//
// Expected structure:
//   --boundary
//   Content-Disposition: form-data; name="request"
//   Content-Type: application/json
//
//   {"text_input": "...", "parameters": {...}}
//
//   --boundary
//   Content-Disposition: form-data; name="image_0"
//   Content-Type: image/jpeg
//
//   <raw JPEG bytes>
//   --boundary--
// ─────────────────────────────────────────────────────────────────────────────

#include <string>
#include <vector>
#include <unordered_map>
#include <stdexcept>

struct MultipartParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct MultipartPart {
    std::string name;
    std::string content_type;
    std::string filename;
    std::vector<uint8_t> data;
};

class MultipartParser {
public:
    /**
     * Extract the boundary string from a Content-Type header value.
     * e.g. "multipart/form-data; boundary=----WebKitFormBoundary7MA4YWxkTrZu0gW"
     * returns "----WebKitFormBoundary7MA4YWxkTrZu0gW"
     *
     * @throws MultipartParseError if boundary is not found
     */
    static std::string extractBoundary(const std::string& content_type);

    /**
     * Parse a multipart/form-data body into named parts.
     *
     * @param body      Raw HTTP request body
     * @param boundary  Boundary string (from extractBoundary())
     * @return          Map of part name → MultipartPart
     * @throws          MultipartParseError on malformed input
     */
    static std::unordered_map<std::string, MultipartPart>
    parse(const std::string& body, const std::string& boundary);

    /**
     * Check if a Content-Type header indicates multipart/form-data.
     */
    static bool isMultipart(const std::string& content_type);
};
