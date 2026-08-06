// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "oip/MultipartParser.h"
#include <algorithm>
#include <sstream>

// ─────────────────────────────────────────────────────────────────────────────
// isMultipart
// ─────────────────────────────────────────────────────────────────────────────
bool MultipartParser::isMultipart(const std::string& content_type) {
    return content_type.find("multipart/form-data") != std::string::npos;
}

// ─────────────────────────────────────────────────────────────────────────────
// extractBoundary
// ─────────────────────────────────────────────────────────────────────────────
std::string MultipartParser::extractBoundary(const std::string& content_type) {
    const std::string key = "boundary=";
    auto pos = content_type.find(key);
    if (pos == std::string::npos) {
        throw MultipartParseError("No boundary found in Content-Type: " + content_type);
    }
    std::string boundary = content_type.substr(pos + key.size());
    // Strip trailing whitespace or semicolons
    while (!boundary.empty() && (boundary.back() == ' ' || boundary.back() == ';')) {
        boundary.pop_back();
    }
    // Strip surrounding quotes if present
    if (boundary.size() >= 2 && boundary.front() == '"' && boundary.back() == '"') {
        boundary = boundary.substr(1, boundary.size() - 2);
    }
    return boundary;
}

// ─────────────────────────────────────────────────────────────────────────────
// parse — split body into parts by boundary
// ─────────────────────────────────────────────────────────────────────────────
std::unordered_map<std::string, MultipartPart>
MultipartParser::parse(const std::string& body, const std::string& boundary) {
    std::unordered_map<std::string, MultipartPart> parts;

    const std::string delim = "--" + boundary;
    const std::string end_delim = "--" + boundary + "--";

    size_t pos = 0;
    while (pos < body.size()) {
        // Find next boundary
        size_t bound_pos = body.find(delim, pos);
        if (bound_pos == std::string::npos) break;

        // Skip past the boundary line (including CRLF)
        size_t after_bound = bound_pos + delim.size();
        if (after_bound >= body.size()) break;

        // Check for end boundary
        if (body.substr(after_bound, 2) == "--") break;

        // Skip CRLF after boundary
        if (after_bound + 1 < body.size() && body[after_bound] == '\r') after_bound++;
        if (after_bound < body.size() && body[after_bound] == '\n') after_bound++;

        // Find end of headers (blank line = \r\n\r\n or \n\n)
        size_t header_end = body.find("\r\n\r\n", after_bound);
        size_t data_start;
        if (header_end != std::string::npos && header_end < body.find(delim, after_bound)) {
            data_start = header_end + 4;
        } else {
            header_end = body.find("\n\n", after_bound);
            if (header_end == std::string::npos) break;
            data_start = header_end + 2;
        }

        // Parse headers
        std::string headers_str = body.substr(after_bound, header_end - after_bound);
        MultipartPart part;

        // Parse Content-Disposition
        std::istringstream hstream(headers_str);
        std::string line;
        while (std::getline(hstream, line)) {
            // Strip \r
            if (!line.empty() && line.back() == '\r') line.pop_back();

            if (line.find("Content-Disposition:") == 0 ||
                line.find("content-disposition:") == 0) {
                // Extract name="..."
                auto name_pos = line.find("name=\"");
                if (name_pos != std::string::npos) {
                    name_pos += 6;
                    auto name_end = line.find('"', name_pos);
                    if (name_end != std::string::npos) {
                        part.name = line.substr(name_pos, name_end - name_pos);
                    }
                }
                // Extract filename="..."
                auto fn_pos = line.find("filename=\"");
                if (fn_pos != std::string::npos) {
                    fn_pos += 10;
                    auto fn_end = line.find('"', fn_pos);
                    if (fn_end != std::string::npos) {
                        part.filename = line.substr(fn_pos, fn_end - fn_pos);
                    }
                }
            } else if (line.find("Content-Type:") == 0 ||
                       line.find("content-type:") == 0) {
                auto ct_pos = line.find(':');
                if (ct_pos != std::string::npos) {
                    part.content_type = line.substr(ct_pos + 1);
                    // Strip leading whitespace
                    while (!part.content_type.empty() && part.content_type.front() == ' ') {
                        part.content_type.erase(part.content_type.begin());
                    }
                }
            }
        }

        // Find end of data (next boundary)
        size_t data_end = body.find("\r\n" + delim, data_start);
        if (data_end == std::string::npos) {
            data_end = body.find("\n" + delim, data_start);
            if (data_end == std::string::npos) {
                data_end = body.size();
            }
        }

        // Copy data bytes
        part.data.assign(
            reinterpret_cast<const uint8_t*>(body.data() + data_start),
            reinterpret_cast<const uint8_t*>(body.data() + data_end));

        if (!part.name.empty()) {
            parts[part.name] = std::move(part);
        }

        pos = data_end;
    }

    return parts;
}
