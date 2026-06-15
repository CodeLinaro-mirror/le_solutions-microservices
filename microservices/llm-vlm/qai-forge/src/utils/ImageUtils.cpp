// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

// ─────────────────────────────────────────────────────────────────────────────
// ImageUtils — Image download, validation, and VLM preprocessing
//
// Common preprocessing pipeline (model-agnostic):
//   1. Decode JPEG/PNG/WEBP/BMP/GIF via stb_image (header-only, no runtime deps)
//   2. Letterbox to target size (default 504×336), preserving aspect ratio
//   3. Floor dimensions to factor multiples (factor = patch_size × merge_size)
//   4. Bilinear resize to floored dimensions
//   5. Normalize: (pixel/255 − mean) / std → CHW float32
//
// Model-specific patch ordering (delegated to VisionPreprocessAdapter):
//   6. Reshape + transpose to (L, D) patch tensor
//      - Qwen models (Qwen2-VL, Qwen2.5-VL, Qwen3-VL):
//          Block-major ordering via QwenVisionPreprocessAdapter
//      - All other VLM models:
//          Row-major ordering via DefaultVisionPreprocessAdapter
//
// The adapter is selected automatically from model_id by
// VisionPreprocessAdapterFactory::getAdapter(model_id).
//
// The preprocessed float32 data is written to a temp file and the path is
// passed to the VLM worker subprocess, which loads it and passes it directly
// to GenieNode_setData(GENIE_NODE_IMAGE_ENCODER_IMAGE_INPUT, ...).
// ─────────────────────────────────────────────────────────────────────────────

// stb_image — header-only image decoder (JPEG, PNG, WEBP, BMP, GIF, TGA, PSD)
// Define the implementation in exactly one .cpp file.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include "stb_image.h"

#include "qai_forge/utils/ImageUtils.h"
#include "qai_forge/adapters/VisionPreprocessAdapterFactory.h"
#include "qai_forge/InternalDTOs.h"
#include "qai_forge/utils/Logger.h"
#include <nlohmann/json.hpp>

#include <sstream>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <cmath>
#include <cstring>
#include <cerrno>
#include <unistd.h>

// libcurl for HTTP image download
#ifdef HAVE_CURL
#include <curl/curl.h>
#endif

// POSIX temp file creation
#include <cstdio>

// Note: InternalDTOs.h defines `using json = nlohmann::ordered_json`.
// ImageUtils uses nlohmann::ordered_json explicitly to avoid redefinition.

// ─────────────────────────────────────────────────────────────────────────────
// Base64 encoding table (used for URL-downloaded images)
// ─────────────────────────────────────────────────────────────────────────────
static const std::string BASE64_CHARS =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64Encode(const std::vector<uint8_t>& data) {
    std::string result;
    result.reserve(((data.size() + 2) / 3) * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t b = (data[i] << 16);
        if (i + 1 < data.size()) b |= (data[i + 1] << 8);
        if (i + 2 < data.size()) b |= data[i + 2];
        result += BASE64_CHARS[(b >> 18) & 0x3F];
        result += BASE64_CHARS[(b >> 12) & 0x3F];
        result += (i + 1 < data.size()) ? BASE64_CHARS[(b >> 6) & 0x3F] : '=';
        result += (i + 2 < data.size()) ? BASE64_CHARS[b & 0x3F] : '=';
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal: bilinear resize (RGB, uint8)
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<uint8_t> bilinearResize(
    const uint8_t* src, int src_w, int src_h,
    int dst_w, int dst_h) {

    std::vector<uint8_t> dst(dst_w * dst_h * 3);
    const float x_ratio = (float)src_w / dst_w;
    const float y_ratio = (float)src_h / dst_h;

    for (int y = 0; y < dst_h; ++y) {
        const float src_y = y * y_ratio;
        const int   y0    = (int)src_y;
        const int   y1    = std::min(y0 + 1, src_h - 1);
        const float fy    = src_y - y0;

        for (int x = 0; x < dst_w; ++x) {
            const float src_x = x * x_ratio;
            const int   x0    = (int)src_x;
            const int   x1    = std::min(x0 + 1, src_w - 1);
            const float fx    = src_x - x0;

            for (int c = 0; c < 3; ++c) {
                const float p00 = src[(y0 * src_w + x0) * 3 + c];
                const float p01 = src[(y0 * src_w + x1) * 3 + c];
                const float p10 = src[(y1 * src_w + x0) * 3 + c];
                const float p11 = src[(y1 * src_w + x1) * 3 + c];
                const float val = p00 * (1.0f - fx) * (1.0f - fy)
                                + p01 * fx           * (1.0f - fy)
                                + p10 * (1.0f - fx)  * fy
                                + p11 * fx           * fy;
                dst[(y * dst_w + x) * 3 + c] =
                    static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, val)));
            }
        }
    }
    return dst;
}

// ─────────────────────────────────────────────────────────────────────────────
// VisionPreprocessConfig
// ─────────────────────────────────────────────────────────────────────────────
VisionPreprocessConfig VisionPreprocessConfig::defaults() {
    return VisionPreprocessConfig{};
}

VisionPreprocessConfig VisionPreprocessConfig::fromJson(const nlohmann::ordered_json& j) {
    VisionPreprocessConfig cfg;

    // patch_size
    if (j.contains("patch_size"))
        cfg.patch_size = j["patch_size"].get<int>();

    // merge_size (also accept spatial_merge_size)
    if (j.contains("merge_size"))
        cfg.merge_size = j["merge_size"].get<int>();
    else if (j.contains("spatial_merge_size"))
        cfg.merge_size = j["spatial_merge_size"].get<int>();

    // temporal_patch_size
    if (j.contains("temporal_patch_size"))
        cfg.temporal_patch_size = j["temporal_patch_size"].get<int>();

    // target_width (also accept image_width)
    if (j.contains("target_width"))
        cfg.target_width = j["target_width"].get<int>();
    else if (j.contains("image_width"))
        cfg.target_width = j["image_width"].get<int>();

    // target_height (also accept image_height)
    if (j.contains("target_height"))
        cfg.target_height = j["target_height"].get<int>();
    else if (j.contains("image_height"))
        cfg.target_height = j["image_height"].get<int>();

    // image_mean (also accept normalize_mean)
    auto parse_mean = [&](const std::string& key) {
        if (j.contains(key) && j[key].is_array() && j[key].size() == 3) {
            cfg.image_mean[0] = j[key][0].get<float>();
            cfg.image_mean[1] = j[key][1].get<float>();
            cfg.image_mean[2] = j[key][2].get<float>();
            return true;
        }
        return false;
    };
    if (!parse_mean("image_mean")) parse_mean("normalize_mean");

    // image_std (also accept normalize_std)
    auto parse_std = [&](const std::string& key) {
        if (j.contains(key) && j[key].is_array() && j[key].size() == 3) {
            cfg.image_std[0] = j[key][0].get<float>();
            cfg.image_std[1] = j[key][1].get<float>();
            cfg.image_std[2] = j[key][2].get<float>();
            return true;
        }
        return false;
    };
    if (!parse_std("image_std")) parse_std("normalize_std");

    return cfg;
}

// ─────────────────────────────────────────────────────────────────────────────
// PreprocessedImage::toBytes
// ─────────────────────────────────────────────────────────────────────────────
std::vector<uint8_t> PreprocessedImage::toBytes() const {
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(pixel_values.data());
    return std::vector<uint8_t>(ptr, ptr + pixel_values.size() * sizeof(float));
}

// ─────────────────────────────────────────────────────────────────────────────
// isBase64DataUri
// ─────────────────────────────────────────────────────────────────────────────
bool ImageUtils::isBase64DataUri(const std::string& url) {
    std::string lower = url.substr(0, std::min(url.size(), size_t(20)));
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.find("data:image") == 0 && url.find(";base64,") != std::string::npos;
}

// ─────────────────────────────────────────────────────────────────────────────
// isRawBase64
// ─────────────────────────────────────────────────────────────────────────────
bool ImageUtils::isRawBase64(const std::string& url) {
    if (url.size() <= BASE64_PREFIX_LENGTH) return false;
    if (url.substr(0, 4) == "http" || url.substr(0, 4) == "data") return false;
    static const std::string allowed =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=_-\n\r";
    for (char c : url.substr(0, 100)) {
        if (allowed.find(c) == std::string::npos) return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseDataUri
// ─────────────────────────────────────────────────────────────────────────────
ImageData ImageUtils::parseDataUri(const std::string& data_uri) {
    ImageData result;
    size_t colon    = data_uri.find(':');
    size_t semicolon = data_uri.find(';');
    size_t comma    = data_uri.find(',');
    if (colon == std::string::npos || semicolon == std::string::npos ||
        comma == std::string::npos) {
        throw std::runtime_error("Invalid data URI format");
    }
    result.mime_type   = data_uri.substr(colon + 1, semicolon - colon - 1);
    result.base64_data = data_uri.substr(comma + 1);
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// downloadImage — HTTP/HTTPS download with browser-like headers
// ─────────────────────────────────────────────────────────────────────────────
#ifdef HAVE_CURL
static size_t curlWriteCallback(void* contents, size_t size, size_t nmemb,
                                 std::vector<uint8_t>* buffer) {
    size_t total = size * nmemb;
    uint8_t* data = static_cast<uint8_t*>(contents);
    buffer->insert(buffer->end(), data, data + total);
    return total;
}
#endif

ImageData ImageUtils::downloadImage(const std::string& url) {
    ImageData result;

#ifdef HAVE_CURL
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw GenAIException(GenAIErrorCode::INVALID_REQUEST,
            "Unable to download image from URL: " + url + " (curl init failed)", 400);
    }

    std::vector<uint8_t> buffer;
    char error_buf[CURL_ERROR_SIZE] = {};

    // Browser-like headers to avoid 403 Forbidden (matches Python requests headers)
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: image/webp,image/apng,image/*,*/*;q=0.8");
    headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.9");
    headers = curl_slist_append(headers, "Connection: keep-alive");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
        "Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36");

    // Enforce size limit during download
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
        static_cast<curl_off_t>(MAX_IMAGE_SIZE_BYTES));

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    // Detect MIME type from Content-Type header
    char* content_type = nullptr;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);
    if (content_type) {
        std::string ct(content_type);
        // Validate it's actually an image
        if (ct.find("image/") != std::string::npos) {
            result.mime_type = ct.substr(0, ct.find(';'));
        } else if (!ct.empty() && ct.find("application/octet-stream") == std::string::npos) {
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            throw GenAIException(GenAIErrorCode::INVALID_REQUEST,
                "URL does not point to an image. Content-Type: " + ct, 400);
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code < 200 || http_code >= 300) {
        std::string err = (res != CURLE_OK)
            ? std::string(error_buf)
            : "HTTP " + std::to_string(http_code);
        throw GenAIException(GenAIErrorCode::INVALID_REQUEST,
            "Unable to download image from URL: " + url + " (" + err + ")", 400);
    }

    if (buffer.empty()) {
        throw GenAIException(GenAIErrorCode::INVALID_REQUEST,
            "Unable to download image from URL: " + url + " (empty response)", 400);
    }

    // Validate size after download
    validateImageSize(buffer.size());

    result.base64_data = base64Encode(buffer);
    if (result.mime_type.empty()) result.mime_type = "image/jpeg";
    return result;

#else
    throw GenAIException(GenAIErrorCode::INVALID_REQUEST,
        "Unable to download image from URL: " + url +
        ". Please provide the image as a base64 data URI instead "
        "(data:image/jpeg;base64,...). URL download requires libcurl.", 400);
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// detectImageFormat — magic bytes detection
// ─────────────────────────────────────────────────────────────────────────────
std::string ImageUtils::detectImageFormat(const std::vector<uint8_t>& data) {
    if (data.size() < 4) return "";

    // JPEG: FF D8 FF
    if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
        return "image/jpeg";

    // PNG: 89 50 4E 47 0D 0A 1A 0A
    if (data.size() >= 8 &&
        data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47 &&
        data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A)
        return "image/png";

    // GIF: 47 49 46 38
    if (data[0] == 0x47 && data[1] == 0x49 && data[2] == 0x46 && data[3] == 0x38)
        return "image/gif";

    // WEBP: 52 49 46 46 ... 57 45 42 50
    if (data.size() >= 12 &&
        data[0] == 0x52 && data[1] == 0x49 && data[2] == 0x46 && data[3] == 0x46 &&
        data[8] == 0x57 && data[9] == 0x45 && data[10] == 0x42 && data[11] == 0x50)
        return "image/webp";

    // BMP: 42 4D
    if (data[0] == 0x42 && data[1] == 0x4D)
        return "image/bmp";

    return "";
}

// ─────────────────────────────────────────────────────────────────────────────
// validateImageFormat
// ─────────────────────────────────────────────────────────────────────────────
void ImageUtils::validateImageFormat(const std::vector<uint8_t>& data) {
    std::string fmt = detectImageFormat(data);
    if (fmt.empty()) {
        throw GenAIException(GenAIErrorCode::INVALID_REQUEST,
            "Unsupported image format. Supported formats: JPEG, PNG, WEBP, GIF, BMP.", 400);
    }
    // Reject animated GIFs (stb_image only loads the first frame anyway, but
    // we match the Python behaviour of explicitly rejecting them)
    // Note: detecting animated GIF requires parsing the GIF header; for now
    // we accept all GIFs and let stb_image handle the first frame.
}

// ─────────────────────────────────────────────────────────────────────────────
// validateImageSize
// ─────────────────────────────────────────────────────────────────────────────
void ImageUtils::validateImageSize(size_t size_bytes) {
    if (size_bytes > MAX_IMAGE_SIZE_BYTES) {
        float size_mb = static_cast<float>(size_bytes) / (1024.0f * 1024.0f);
        throw GenAIException(GenAIErrorCode::INVALID_REQUEST,
            "Image exceeds size limit. Size: " + std::to_string(size_mb) +
            " MB, Limit: " + std::to_string(MAX_IMAGE_SIZE_MB) + " MB.", 400);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveImage
// ─────────────────────────────────────────────────────────────────────────────
ImageData ImageUtils::resolveImage(const std::string& url) {
    if (url.empty()) {
        throw GenAIException(GenAIErrorCode::INVALID_REQUEST,
            "Image URL is empty.", 400);
    }

    if (isBase64DataUri(url)) {
        LOG_DEBUG("[ImageUtils] Extracting base64 from data URI");
        return parseDataUri(url);
    }

    if (isRawBase64(url)) {
        LOG_DEBUG("[ImageUtils] Using raw base64 image data");
        ImageData result;
        result.base64_data = url;
        result.mime_type   = "image/jpeg";
        return result;
    }

    if (url.substr(0, 4) == "http") {
        LOG_INFO("[ImageUtils] Downloading image from URL: "
                 << url.substr(0, 80) << (url.size() > 80 ? "..." : ""));
        return downloadImage(url);
    }

    throw GenAIException(GenAIErrorCode::INVALID_REQUEST,
        "Unsupported image format. Provide a URL (https://...) or "
        "base64 data URI (data:image/...;base64,...)", 400);
}

// ─────────────────────────────────────────────────────────────────────────────
// compactBase64 / isCompacted
// ─────────────────────────────────────────────────────────────────────────────
std::string ImageUtils::compactBase64(const std::string& base64_data) {
    if (base64_data.size() <= BASE64_PREFIX_LENGTH) return base64_data;
    return base64_data.substr(0, BASE64_PREFIX_LENGTH);
}

bool ImageUtils::isCompacted(const std::string& base64_data) {
    return base64_data.size() <= BASE64_PREFIX_LENGTH;
}

// ─────────────────────────────────────────────────────────────────────────────
// base64Decode
// ─────────────────────────────────────────────────────────────────────────────
std::vector<uint8_t> ImageUtils::base64Decode(const std::string& encoded) {
    static const int8_t decode_table[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };

    std::vector<uint8_t> result;
    result.reserve((encoded.size() / 4) * 3);

    int val = 0, valb = -8;
    for (unsigned char c : encoded) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int8_t d = decode_table[c];
        if (d < 0) continue;
        val = (val << 6) | d;
        valb += 6;
        if (valb >= 0) {
            result.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// writeTempImageFile — write raw compressed bytes to a temp file
// ─────────────────────────────────────────────────────────────────────────────
std::string ImageUtils::writeTempImageFile(const std::vector<uint8_t>& data,
                                            const std::string& mime_type) {
    std::string ext = ".jpg";
    if (mime_type == "image/png")  ext = ".png";
    else if (mime_type == "image/gif")  ext = ".gif";
    else if (mime_type == "image/webp") ext = ".webp";
    else if (mime_type == "image/bmp")  ext = ".bmp";
    else if (mime_type == "image/tiff") ext = ".tiff";

    std::string tmpl = "/tmp/vlm_img_XXXXXX" + ext;
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');

    int fd = ::mkstemps(buf.data(), static_cast<int>(ext.size()));
    if (fd < 0) {
        throw std::runtime_error(
            std::string("Failed to create temp image file: ") + strerror(errno));
    }

    std::string path(buf.data());
    const uint8_t* ptr = data.data();
    size_t remaining = data.size();
    while (remaining > 0) {
        ssize_t written = ::write(fd, ptr, remaining);
        if (written < 0) {
            ::close(fd);
            ::unlink(path.c_str());
            throw std::runtime_error(
                std::string("Failed to write temp image file: ") + strerror(errno));
        }
        ptr += written;
        remaining -= static_cast<size_t>(written);
    }
    ::close(fd);

    LOG_DEBUG("[ImageUtils] Wrote temp image file: " << path
              << " (" << data.size() << " bytes, " << mime_type << ")");
    return path;
}

// ─────────────────────────────────────────────────────────────────────────────
// resolveToTempFile — resolve URL/base64 to raw compressed temp file
// ─────────────────────────────────────────────────────────────────────────────
std::string ImageUtils::resolveToTempFile(const std::string& url) {
    LOG_DEBUG("[ImageUtils] Resolving image to temp file...");
    ImageData img = resolveImage(url);
    std::vector<uint8_t> binary = base64Decode(img.base64_data);
    if (binary.empty()) {
        throw GenAIException(GenAIErrorCode::INVALID_REQUEST,
            "Image decoded to empty binary data — check that the base64 payload is valid.", 400);
    }
    validateImageSize(binary.size());
    validateImageFormat(binary);
    return writeTempImageFile(binary, img.mime_type);
}

// ─────────────────────────────────────────────────────────────────────────────
// preprocessImage — full VLM preprocessing pipeline
//
// Common steps (model-agnostic):
//   1. Decode JPEG/PNG/WEBP/BMP/GIF via stb_image
//   2. Letterbox to target size, preserving aspect ratio
//   3. Floor dimensions to factor multiples (patch_size × merge_size)
//   4. Bilinear resize to floored dimensions
//   5. Normalize: (pixel/255 − mean) / std → CHW float32
//
// Model-specific step (delegated to VisionPreprocessAdapter):
//   6. Reshape + transpose to (L, D) patch tensor
//      - Qwen models: block-major patch ordering
//      - Default:     row-major patch ordering
// ─────────────────────────────────────────────────────────────────────────────
PreprocessedImage ImageUtils::preprocessImage(
    const std::vector<uint8_t>& compressed_data,
    const VisionPreprocessConfig& cfg,
    const std::string& model_id) {

    // ── 0. Validate size and format ───────────────────────────────────────────
    validateImageSize(compressed_data.size());
    validateImageFormat(compressed_data);

    // ── 1. Decode image via stb_image (forces RGB output) ─────────────────────
    int orig_w = 0, orig_h = 0, channels = 0;
    uint8_t* decoded = stbi_load_from_memory(
        compressed_data.data(),
        static_cast<int>(compressed_data.size()),
        &orig_w, &orig_h, &channels,
        3  // force 3-channel RGB
    );
    if (!decoded) {
        throw std::runtime_error(
            std::string("[ImageUtils] Failed to decode image: ") +
            (stbi_failure_reason() ? stbi_failure_reason() : "unknown error"));
    }

    LOG_DEBUG("[ImageUtils] Decoded image: " << orig_w << "x" << orig_h
              << " (original channels=" << channels << ")");

    // ── 2. Letterbox to target size ───────────────────────────────────────────
    // Scale to fit preserving aspect ratio, then center-pad with black.
    const float scale = std::min(
        static_cast<float>(cfg.target_width)  / orig_w,
        static_cast<float>(cfg.target_height) / orig_h
    );
    const int fit_w = static_cast<int>(orig_w * scale);
    const int fit_h = static_cast<int>(orig_h * scale);

    LOG_DEBUG("[ImageUtils] Letterbox: scale=" << scale
              << " fit=" << fit_w << "x" << fit_h
              << " target=" << cfg.target_width << "x" << cfg.target_height);

    // Resize to fit_w x fit_h
    std::vector<uint8_t> resized = bilinearResize(decoded, orig_w, orig_h, fit_w, fit_h);
    stbi_image_free(decoded);
    decoded = nullptr;

    // Create padded image (target_width x target_height, black background)
    std::vector<uint8_t> padded(cfg.target_width * cfg.target_height * 3, 0);
    const int offset_x = (cfg.target_width  - fit_w) / 2;
    const int offset_y = (cfg.target_height - fit_h) / 2;

    for (int y = 0; y < fit_h; ++y) {
        const int dst_row = (offset_y + y) * cfg.target_width;
        const int src_row = y * fit_w;
        for (int x = 0; x < fit_w; ++x) {
            for (int c = 0; c < 3; ++c) {
                padded[(dst_row + offset_x + x) * 3 + c] =
                    resized[(src_row + x) * 3 + c];
            }
        }
    }

    // ── 3. Floor dimensions to factor multiples ───────────────────────────────
    const int factor = cfg.patch_size * cfg.merge_size;
    const int new_w  = (cfg.target_width  / factor) * factor;
    const int new_h  = (cfg.target_height / factor) * factor;

    LOG_DEBUG("[ImageUtils] Floor to factor(" << factor << "): "
              << cfg.target_width << "x" << cfg.target_height
              << " → " << new_w << "x" << new_h);

    if (new_w < factor || new_h < factor) {
        throw std::runtime_error(
            "[ImageUtils] Image too small after flooring: " +
            std::to_string(new_w) + "x" + std::to_string(new_h) +
            " (needs >= " + std::to_string(factor) + " on each side)");
    }

    // Resize padded to new_w x new_h
    std::vector<uint8_t> final_img = bilinearResize(
        padded.data(), cfg.target_width, cfg.target_height, new_w, new_h);

    // ── 4. Normalize and build CHW float32 array ──────────────────────────────
    // chw[c, h, w] = (pixel[h, w, c] / 255.0 - mean[c]) / std[c]
    const int C = 3;
    const int H = new_h;
    const int W = new_w;

    std::vector<float> chw(C * H * W);
    for (int h = 0; h < H; ++h) {
        for (int w = 0; w < W; ++w) {
            for (int c = 0; c < C; ++c) {
                const float pixel = final_img[(h * W + w) * 3 + c] / 255.0f;
                chw[c * H * W + h * W + w] =
                    (pixel - cfg.image_mean[c]) / cfg.image_std[c];
            }
        }
    }

    // ── 5. Compute grid dimensions ────────────────────────────────────────────
    const int grid_t = 1;                          // single temporal group for static images
    const int grid_h = H / cfg.patch_size;
    const int grid_w = W / cfg.patch_size;
    const int L      = grid_t * grid_h * grid_w;
    const int D      = C * cfg.temporal_patch_size * cfg.patch_size * cfg.patch_size;

    LOG_DEBUG("[ImageUtils] Grid: grid_t=" << grid_t
              << " grid_h=" << grid_h << " grid_w=" << grid_w
              << " L=" << L << " D=" << D
              << " adapter=" << VisionPreprocessAdapterFactory::getAdapter(model_id).getName());

    // ── 6. Model-specific patch ordering via adapter ──────────────────────────
    // Delegates temporal duplication + reshape + transpose to the adapter
    // selected from model_id (Qwen: block-major, Default: row-major).
    const VisionPreprocessAdapter& adapter =
        VisionPreprocessAdapterFactory::getAdapter(model_id);
    std::vector<float> flat = adapter.preprocessPatches(chw, W, H, cfg);

    LOG_INFO("[ImageUtils] Preprocessed image: "
             << orig_w << "x" << orig_h << " → " << new_w << "x" << new_h
             << " patches=(" << L << "," << D << ")"
             << " adapter=" << adapter.getName()
             << " buffer=" << (flat.size() * sizeof(float)) << " bytes");

    PreprocessedImage result;
    result.pixel_values    = std::move(flat);
    result.grid_t          = grid_t;
    result.grid_h          = grid_h;
    result.grid_w          = grid_w;
    result.num_patches     = L;
    result.patch_dim       = D;
    result.original_width  = orig_w;
    result.original_height = orig_h;
    result.resized_width   = new_w;
    result.resized_height  = new_h;
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// preprocessImageToTempFile — download + preprocess + write .raw temp file
// ─────────────────────────────────────────────────────────────────────────────
std::string ImageUtils::preprocessImageToTempFile(
    const std::string& url,
    const VisionPreprocessConfig& config,
    const std::string& model_id) {

    LOG_DEBUG("[ImageUtils] Preprocessing image to temp file: "
              << url.substr(0, 80) << (url.size() > 80 ? "..." : ""));

    // 1. Download / decode to raw compressed bytes
    ImageData img = resolveImage(url);
    std::vector<uint8_t> compressed = base64Decode(img.base64_data);
    if (compressed.empty()) {
        throw GenAIException(GenAIErrorCode::INVALID_REQUEST,
            "Image decoded to empty binary data — check that the base64 payload is valid.", 400);
    }

    // 2. Preprocess to float32 (L, D) tensor using model-specific adapter
    PreprocessedImage preprocessed = preprocessImage(compressed, config, model_id);

    // 3. Write float32 bytes to a temp file with .raw extension
    std::vector<uint8_t> raw_bytes = preprocessed.toBytes();

    std::string tmpl = "/tmp/vlm_pre_XXXXXX.raw";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');

    int fd = ::mkstemps(buf.data(), 4 /* ".raw" */);
    if (fd < 0) {
        throw std::runtime_error(
            std::string("Failed to create preprocessed image temp file: ") + strerror(errno));
    }

    std::string path(buf.data());
    const uint8_t* ptr = raw_bytes.data();
    size_t remaining = raw_bytes.size();
    while (remaining > 0) {
        ssize_t written = ::write(fd, ptr, remaining);
        if (written < 0) {
            ::close(fd);
            ::unlink(path.c_str());
            throw std::runtime_error(
                std::string("Failed to write preprocessed image temp file: ") + strerror(errno));
        }
        ptr += written;
        remaining -= static_cast<size_t>(written);
    }
    ::close(fd);

    LOG_INFO("[ImageUtils] Wrote preprocessed image: " << path
             << " (" << raw_bytes.size() << " bytes"
             << ", patches=" << preprocessed.num_patches
             << "x" << preprocessed.patch_dim << ")");
    return path;
}

// ─────────────────────────────────────────────────────────────────────────────
// TempFileGuard::~TempFileGuard
// ─────────────────────────────────────────────────────────────────────────────
ImageUtils::TempFileGuard::~TempFileGuard() {
    for (const auto& path : paths) {
        if (!path.empty()) {
            if (::unlink(path.c_str()) != 0) {
                LOG_WARN("[ImageUtils] Failed to delete temp file: " << path
                         << " (" << strerror(errno) << ")");
            } else {
                LOG_DEBUG("[ImageUtils] Deleted temp file: " << path);
            }
        }
    }
}
