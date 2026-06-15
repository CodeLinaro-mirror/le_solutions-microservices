// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#pragma once

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <cstdint>

// Use the official nlohmann forward-declaration header.
// This provides the correct `using json = basic_json<>` alias without pulling
// in the full JSON implementation.  Callers that use VisionPreprocessConfig::fromJson()
// do not need to include nlohmann/json.hpp separately.
#include <nlohmann/json_fwd.hpp>

// ─────────────────────────────────────────────────────────────────────────────
// ImageUtils — Image download, validation, and preprocessing for VLM requests
//
// Handles two image input formats from the OpenAI API:
//   1. URL:    "image_url": {"url": "https://..."}
//              → Downloads the image with proper HTTP headers.
//              → Validates size (≤50 MB) and format (JPEG/PNG/WEBP/GIF).
//
//   2. Base64: "image_url": {"url": "data:image/jpeg;base64,/9j/4AAQ..."}
//              → Extracts the raw base64 payload directly.
//
// VLM Preprocessing (preprocessImage / preprocessImagesToTempFiles):
//   Implements the full Qwen2.5-VL preprocessing pipeline in C++, matching
//   the Python image_preprocessor.py exactly:
//     1. Decode JPEG/PNG/WEBP/BMP/GIF via stb_image
//     2. Letterbox to target size (default 512×342), preserving aspect ratio
//     3. Floor dimensions to factor multiples (factor = patch_size × merge_size)
//     4. Bilinear resize to floored dimensions
//     5. Normalize: (pixel/255 − mean) / std
//     6. Transpose to CHW format
//     7. Duplicate to temporal_patch_size frames
//     8. Reshape + transpose to (L, D) patch tensor
//     9. Write float32 little-endian binary to temp file
//
//   The preprocessed float32 data is written to a temp file and the path is
//   passed to the VLM worker subprocess, which loads it and passes it directly
//   to GenieNode_setData(GENIE_NODE_IMAGE_ENCODER_IMAGE_INPUT, ...).
//
// Session storage optimization:
//   Full base64 image data is stored only for the LATEST image; older images
//   are compacted to BASE64_PREFIX_LENGTH characters to bound session size.
// ─────────────────────────────────────────────────────────────────────────────

// Number of base64 characters to keep for historical images (compaction)
static constexpr size_t BASE64_PREFIX_LENGTH = 256;

// Maximum image size accepted (50 MB)
static constexpr size_t MAX_IMAGE_SIZE_MB    = 50;
static constexpr size_t MAX_IMAGE_SIZE_BYTES = MAX_IMAGE_SIZE_MB * 1024 * 1024;

// ─────────────────────────────────────────────────────────────────────────────
// ImageData — result of resolveImage() / downloadImage()
// ─────────────────────────────────────────────────────────────────────────────
struct ImageData {
    std::string base64_data;    // Raw base64-encoded image bytes (no data: prefix)
    std::string mime_type;      // e.g. "image/jpeg", "image/png"
    bool is_compacted = false;  // True if this is a historical compacted image
};

// ─────────────────────────────────────────────────────────────────────────────
// VisionPreprocessConfig — model-specific preprocessing parameters
//
// Read from metadata.json → genie.vision_preprocessing.
// Falls back to Qwen2.5-VL defaults if the field is absent.
// ─────────────────────────────────────────────────────────────────────────────
struct VisionPreprocessConfig {
    int   patch_size          = 14;
    int   merge_size          = 2;
    int   temporal_patch_size = 2;
    int   target_width        = 512;
    int   target_height       = 342;
    float image_mean[3]       = {0.48145466f, 0.45782750f, 0.40821073f};
    float image_std[3]        = {0.26862954f, 0.26130258f, 0.27577711f};

    // Build from the genie.vision_preprocessing JSON object in metadata.json.
    // Accepts both new-style keys (patch_size, merge_size, …) and legacy aliases
    // (spatial_merge_size, image_width, image_height, normalize_mean, normalize_std).
    // Uses ordered_json to match the InternalDTOs.h `using json = nlohmann::ordered_json` alias.
    static VisionPreprocessConfig fromJson(const nlohmann::ordered_json& j);

    // Return the default Qwen2.5-VL config.
    static VisionPreprocessConfig defaults();
};

// ─────────────────────────────────────────────────────────────────────────────
// PreprocessedImage — result of preprocessImage()
//
// pixel_values is a flat float32 array of shape (L, D) in row-major order:
//   L = grid_t × grid_h × grid_w   (number of patches)
//   D = C × temporal_patch_size × patch_size²  (floats per patch)
// ─────────────────────────────────────────────────────────────────────────────
struct PreprocessedImage {
    std::vector<float> pixel_values;  // (L × D) float32, little-endian
    int   grid_t          = 0;
    int   grid_h          = 0;
    int   grid_w          = 0;
    int   num_patches     = 0;   // L
    int   patch_dim       = 0;   // D
    int   original_width  = 0;
    int   original_height = 0;
    int   resized_width   = 0;
    int   resized_height  = 0;

    // Return the raw bytes of pixel_values (for writing to file / IPC).
    std::vector<uint8_t> toBytes() const;
};

// ─────────────────────────────────────────────────────────────────────────────
// ImageUtils
// ─────────────────────────────────────────────────────────────────────────────
class ImageUtils {
public:
    // ── URL / base64 resolution ───────────────────────────────────────────────

    /**
     * Resolve an image URL or base64 data URI to raw base64 bytes.
     * Handles: https://..., data:image/...;base64,..., raw base64 strings.
     * @throws GenAIException(INVALID_REQUEST, ..., 400) on failure.
     */
    static ImageData resolveImage(const std::string& url);

    /** True if url starts with "data:image" and contains ";base64,". */
    static bool isBase64DataUri(const std::string& url);

    /** True if url looks like raw base64 (no http/data prefix, long string). */
    static bool isRawBase64(const std::string& url);

    /** Extract base64 payload and mime type from a data URI. */
    static ImageData parseDataUri(const std::string& data_uri);

    /**
     * Download an image from an HTTP/HTTPS URL.
     * Sends a browser-like User-Agent to avoid 403 Forbidden responses.
     * Validates Content-Type and enforces the 50 MB size limit.
     * @throws GenAIException(INVALID_REQUEST, ..., 400) on failure.
     */
    static ImageData downloadImage(const std::string& url);

    // ── Validation ────────────────────────────────────────────────────────────

    /**
     * Detect the image format from the first few bytes (magic bytes).
     * Returns one of: "image/jpeg", "image/png", "image/webp", "image/gif",
     *                 "image/bmp", or "" (unknown).
     */
    static std::string detectImageFormat(const std::vector<uint8_t>& data);

    /**
     * Validate that the image format is supported (JPEG/PNG/WEBP/GIF/BMP).
     * @throws GenAIException(INVALID_REQUEST, ..., 400) for unsupported formats.
     */
    static void validateImageFormat(const std::vector<uint8_t>& data);

    /**
     * Validate that the image size does not exceed MAX_IMAGE_SIZE_BYTES.
     * @throws GenAIException(INVALID_REQUEST, ..., 400) if too large.
     */
    static void validateImageSize(size_t size_bytes);

    // ── Session storage compaction ────────────────────────────────────────────

    /** Compact a base64 string to the first BASE64_PREFIX_LENGTH characters. */
    static std::string compactBase64(const std::string& base64_data);

    /** True if the base64 string has been compacted. */
    static bool isCompacted(const std::string& base64_data);

    // ── Binary utilities ──────────────────────────────────────────────────────

    /**
     * Decode a base64 string to raw binary bytes.
     * Handles standard base64 with optional whitespace/newlines.
     */
    static std::vector<uint8_t> base64Decode(const std::string& base64_data);

    /**
     * Write binary image data to a temporary file.
     * @param data      Raw binary image bytes.
     * @param mime_type MIME type used to choose file extension.
     * @return          Absolute path to the created temp file.
     * @throws std::runtime_error if the file cannot be created.
     */
    static std::string writeTempImageFile(const std::vector<uint8_t>& data,
                                          const std::string& mime_type);

    /**
     * Resolve an image URL/base64 data URI to a local temporary file
     * containing the raw (compressed) image bytes.
     *
     * This is used for simple file-path passing; for VLM inference use
     * preprocessImageToTempFile() instead.
     */
    static std::string resolveToTempFile(const std::string& url);

    // ── VLM Preprocessing ─────────────────────────────────────────────────────

    /**
     * Preprocess a compressed image (JPEG/PNG/etc.) into the float32 patch
     * tensor expected by the GenIE image encoder node.
     *
     * Implements the full VLM preprocessing pipeline:
     *   decode → letterbox → floor → normalize → CHW →
     *   model-specific patch ordering → (L, D) float32 array
     *
     * The patch ordering step is delegated to the VisionPreprocessAdapter
     * resolved from model_id by VisionPreprocessAdapterFactory:
     *   - Qwen models (qwen2-vl, qwen2.5-vl, qwen3-vl): block-major ordering
     *   - All other models: row-major ordering (default)
     *
     * @param compressed_data  Raw compressed image bytes (JPEG, PNG, etc.)
     * @param config           Model-specific preprocessing parameters.
     * @param model_id         Model identifier used to select the preprocessing
     *                         adapter (e.g. "qwen2.5-vl-7b-instruct").
     * @return                 PreprocessedImage with pixel_values in (L, D) format.
     * @throws std::runtime_error if decoding or preprocessing fails.
     */
    static PreprocessedImage preprocessImage(
        const std::vector<uint8_t>& compressed_data,
        const VisionPreprocessConfig& config,
        const std::string& model_id = "");

    /**
     * Download/decode an image URL and preprocess it, writing the resulting
     * float32 pixel data to a temporary file.
     *
     * This is the primary entry point for the VLM pipeline:
     *   URL/base64 → download/decode → preprocess → write .raw temp file
     *
     * The temp file contains the raw float32 bytes of the (L, D) tensor.
     * The VLM worker loads this file and passes the bytes directly to
     * GenieNode_setData(GENIE_NODE_IMAGE_ENCODER_IMAGE_INPUT, ...).
     *
     * The patch ordering algorithm is selected automatically from model_id:
     *   - Qwen models: block-major ordering (Qwen2-VL / Qwen2.5-VL / Qwen3-VL)
     *   - All other models: row-major ordering (default)
     *
     * @param url      Image URL, data URI, or raw base64 string.
     * @param config   Model-specific preprocessing parameters.
     * @param model_id Model identifier used to select the preprocessing adapter.
     * @return         Absolute path to a temp file containing float32 pixel data.
     *                 The caller must delete this file after use.
     * @throws GenAIException(INVALID_REQUEST, ..., 400) on failure.
     */
    static std::string preprocessImageToTempFile(
        const std::string& url,
        const VisionPreprocessConfig& config,
        const std::string& model_id = "");

    // ── RAII temp file guard ──────────────────────────────────────────────────

    /**
     * RAII helper: holds a list of temp file paths and deletes them on destruction.
     *
     * Example:
     *   ImageUtils::TempFileGuard guard;
     *   for (auto& url : image_urls) {
     *       guard.paths.push_back(
     *           ImageUtils::preprocessImageToTempFile(url, config));
     *   }
     *   // ... pass guard.paths to VLM worker ...
     *   // Files are deleted when guard goes out of scope.
     */
    struct TempFileGuard {
        std::vector<std::string> paths;
        ~TempFileGuard();
    };
};
