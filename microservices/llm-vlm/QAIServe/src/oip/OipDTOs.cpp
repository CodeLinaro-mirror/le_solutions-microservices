// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "oip/OipDTOs.h"
#include <stdexcept>

using json = nlohmann::json;

// ─────────────────────────────────────────────────────────────────────────────
// OipGenerateParameters::fromJson
// ─────────────────────────────────────────────────────────────────────────────
OipGenerateParameters OipGenerateParameters::fromJson(const json& j) {
    OipGenerateParameters p;
    const json& params = j.contains("parameters") && j["parameters"].is_object()
                         ? j["parameters"] : json::object();
    p.max_new_tokens = params.value("max_new_tokens", 512);
    p.temperature    = params.value("temperature",    1.0f);
    p.top_p          = params.value("top_p",          1.0f);
    p.top_k          = params.value("top_k",          40);
    p.stream         = params.value("stream",         false);
    p.user           = params.value("user",           std::string{});
    return p;
}

// ─────────────────────────────────────────────────────────────────────────────
// OipGenerateRequest::fromJson
// ─────────────────────────────────────────────────────────────────────────────
OipGenerateRequest OipGenerateRequest::fromJson(const json& j) {
    OipGenerateRequest r;

    if (j.contains("text_input") && j["text_input"].is_string()) {
        r.text_input = j["text_input"].get<std::string>();
    }
    if (j.contains("messages") && j["messages"].is_array()) {
        r.messages = j["messages"];
    }

    // Base64-encoded images in JSON (alternative to multipart)
    if (j.contains("images") && j["images"].is_array()) {
        for (const auto& img : j["images"]) {
            if (img.is_string()) {
                // base64 decode
                const std::string& b64 = img.get<std::string>();
                // Simple base64 decode
                static const std::string chars =
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                std::vector<uint8_t> decoded;
                decoded.reserve(b64.size() * 3 / 4);
                int val = 0, valb = -8;
                for (unsigned char c : b64) {
                    if (c == '=') break;
                    auto pos = chars.find(c);
                    if (pos == std::string::npos) continue;
                    val = (val << 6) + static_cast<int>(pos);
                    valb += 6;
                    if (valb >= 0) {
                        decoded.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
                        valb -= 8;
                    }
                }
                r.images.push_back(std::move(decoded));
            }
        }
    }

    r.parameters = OipGenerateParameters::fromJson(j);
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// OipInferResponse::toJson
// ─────────────────────────────────────────────────────────────────────────────
json OipInferResponse::toJson() const {
    json j;
    j["model_name"] = model_name;
    j["id"]         = id;
    j["outputs"]    = json::array();
    for (const auto& out : outputs) {
        json o;
        o["name"]     = out.name;
        o["shape"]    = out.shape;
        o["datatype"] = out.datatype;
        // Encode binary data as base64 for JSON response
        // (clients that want raw bytes should use binary extension)
        static const std::string chars =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string b64;
        b64.reserve((out.data.size() + 2) / 3 * 4);
        for (size_t i = 0; i < out.data.size(); i += 3) {
            uint32_t v = static_cast<uint32_t>(out.data[i]) << 16;
            if (i + 1 < out.data.size()) v |= static_cast<uint32_t>(out.data[i+1]) << 8;
            if (i + 2 < out.data.size()) v |= static_cast<uint32_t>(out.data[i+2]);
            b64 += chars[(v >> 18) & 63];
            b64 += chars[(v >> 12) & 63];
            b64 += (i + 1 < out.data.size()) ? chars[(v >> 6) & 63] : '=';
            b64 += (i + 2 < out.data.size()) ? chars[v & 63]        : '=';
        }
        o["data"] = b64;
        j["outputs"].push_back(o);
    }
    return j;
}

// ─────────────────────────────────────────────────────────────────────────────
// OipGenerateResponse::toJson
// ─────────────────────────────────────────────────────────────────────────────
json OipGenerateResponse::toJson() const {
    json j = {
        {"model_name",        model_name},
        {"text_output",       text_output},
        {"finish_reason",     finish_reason},
        {"prompt_tokens",     prompt_tokens},
        {"completion_tokens", completion_tokens},
    };
    if (!reasoning_output.empty()) {
        j["reasoning_output"] = reasoning_output;
    }
    return j;
}

// ─────────────────────────────────────────────────────────────────────────────
// OipStreamChunk::toJson
// ─────────────────────────────────────────────────────────────────────────────
json OipStreamChunk::toJson() const {
    json j = {{"model_name", model_name}};
    if (!text_output.empty())      j["text_output"]      = text_output;
    if (!reasoning_output.empty()) j["reasoning_output"] = reasoning_output;
    if (!finish_reason.empty()) {
        j["finish_reason"]     = finish_reason;
        j["completion_tokens"] = completion_tokens;
    }
    return j;
}
