// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "postprocess-utils.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace {

// IEEE 754 half-precision (FP16) -> single-precision (FP32) bit conversion.
// No hardware FP16 intrinsic dependency — pure bit manipulation so this
// builds identically on every target.
float fp16ToFloat(uint16_t h) {
    uint32_t sign     = (h & 0x8000u) << 16;
    uint32_t exponent  = (h & 0x7C00u) >> 10;
    uint32_t mantissa  = (h & 0x03FFu);
    uint32_t bits;

    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;  // signed zero
        } else {
            // subnormal half -> normalize into a normal float
            exponent = 1;
            while ((mantissa & 0x0400u) == 0) {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x03FFu;
            bits = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
        }
    } else if (exponent == 0x1F) {
        bits = sign | 0x7F800000u | (mantissa << 13);  // Inf/NaN
    } else {
        bits = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
    }

    float out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
}

} // namespace

namespace PostprocessUtils {

float readFloat(const postproc_abi::OutputTensor& t, size_t idx, float quant_scale, int32_t quant_zero_point) {
    switch (t.dtype) {
        case postproc_abi::DataType::UINT8: {
            uint8_t raw = t.data[idx];
            return (static_cast<float>(raw) - static_cast<float>(quant_zero_point)) * quant_scale;
        }
        case postproc_abi::DataType::INT8: {
            int8_t raw = static_cast<int8_t>(t.data[idx]);
            return (static_cast<float>(raw) - static_cast<float>(quant_zero_point)) * quant_scale;
        }
        case postproc_abi::DataType::FLOAT16: {
            uint16_t raw;
            std::memcpy(&raw, t.data + idx * 2, 2);
            return fp16ToFloat(raw);
        }
        case postproc_abi::DataType::FLOAT32:
        default: {
            float raw;
            std::memcpy(&raw, t.data + idx * 4, 4);
            return raw;
        }
    }
}

float getFloat(const postproc_abi::RequestConfig& cfg, const std::string& key, float default_val) {
    auto it = cfg.extra.find(key);
    if (it == cfg.extra.end()) return default_val;
    try {
        return std::stof(it->second);
    } catch (const std::exception&) {
        return default_val;
    }
}

float getFloatInRange(const postproc_abi::RequestConfig& cfg, const std::string& key,
                       float default_val, float lo, float hi) {
    const float val = getFloat(cfg, key, default_val);
    return (val < lo || val > hi) ? default_val : val;
}

std::map<int, std::string> loadLabelsFile(const std::string& labels_path, bool required) {
    std::map<int, std::string> labels;

    if (labels_path.empty()) {
        if (required) throw std::runtime_error("labels file required but model has no labels.txt alongside its config_file");
        return labels;
    }

    std::ifstream f(labels_path);
    if (!f.is_open()) {
        if (required) throw std::runtime_error("labels file not found: " + labels_path);
        return labels;
    }

    std::string line;
    int idx = 0;
    while (std::getline(f, line)) {
        // Strip trailing \r (files saved with CRLF line endings on Windows).
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        labels[idx++] = line;
    }

    if (required && labels.empty()) {
        throw std::runtime_error("labels file is empty: " + labels_path);
    }
    return labels;
}

float nmsIoU(const Detection& a, const Detection& b) {
    const float width  = std::min(a.x2, b.x2) - std::max(a.x1, b.x1);
    if (width <= 0.0f) return 0.0f;
    const float height = std::min(a.y2, b.y2) - std::max(a.y1, b.y1);
    if (height <= 0.0f) return 0.0f;

    const float intersection = width * height;
    const float area_a = (a.x2 - a.x1) * (a.y2 - a.y1);
    const float area_b = (b.x2 - b.x1) * (b.y2 - b.y1);
    return intersection / (area_a + area_b - intersection);
}

} // namespace PostprocessUtils
