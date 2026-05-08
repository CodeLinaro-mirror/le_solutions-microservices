# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import numpy as np
from PIL import Image, ImageOps
from typing import List, Tuple, Optional, Dict
import io

# Default preprocessing constants (Qwen2.5-VL)
DEFAULT_IMAGE_MEAN = np.array([0.48145466, 0.45782750, 0.40821073], dtype=np.float32)
DEFAULT_IMAGE_STD = np.array([0.26862954, 0.26130258, 0.27577711], dtype=np.float32)
DEFAULT_PATCH_SIZE = 14
DEFAULT_MERGE_SIZE = 2
DEFAULT_TEMPORAL_PATCH_SIZE = 2
DEFAULT_TARGET_WIDTH = 512
DEFAULT_TARGET_HEIGHT = 342

# Legacy aliases for backward compatibility
CLIP_MEAN = DEFAULT_IMAGE_MEAN
CLIP_STD = DEFAULT_IMAGE_STD
PATCH_SIZE = DEFAULT_PATCH_SIZE
MERGE_SIZE = DEFAULT_MERGE_SIZE
FACTOR = PATCH_SIZE * MERGE_SIZE  # 28
TEMPORAL_PATCH_SIZE = DEFAULT_TEMPORAL_PATCH_SIZE

class PreprocessedImage:
    """Container for preprocessed image data and metadata."""
    def __init__(self, pixel_values: np.ndarray, grid_t: int, grid_h: int, grid_w: int,
                 original_width: int, original_height: int, resized_width: int, resized_height: int):
        self.pixel_values = pixel_values  # Shape: (L, D) where L = grid_t * grid_h * grid_w
        self.grid_t = grid_t
        self.grid_h = grid_h
        self.grid_w = grid_w
        self.num_patches = pixel_values.shape[0]  # L
        self.patch_dim = pixel_values.shape[1]    # D
        self.original_width = original_width
        self.original_height = original_height
        self.resized_width = resized_width
        self.resized_height = resized_height

    def to_bytes(self) -> bytes:
        """Convert pixel values to raw bytes for C++ interface."""
        return np.ascontiguousarray(self.pixel_values).tobytes()

    def save_to_file(self, filepath: str):
        """Save pixel values to a raw binary file."""
        with open(filepath, "wb") as f:
            np.ascontiguousarray(self.pixel_values).tofile(f)

def preprocess_image(img: Image.Image, vision_config: dict = None) -> PreprocessedImage:
    """
    Preprocess a PIL Image for VLM model inference.

    Args:
        img: PIL Image object in RGB mode
        vision_config: Optional dict with model-specific preprocessing parameters.
            Keys: target_width, target_height, patch_size, merge_size,
                  temporal_patch_size, image_mean, image_std.
            Falls back to Qwen2.5-VL defaults if not provided.

    Returns:
        PreprocessedImage object containing pixel values and metadata

    Raises:
        ValueError: If image is too small after flooring to FACTOR multiples
    """
    import logging
    logger = logging.getLogger(__name__)

    # Extract parameters from vision_config or use defaults
    if vision_config:
        patch_size = vision_config.get('patch_size', DEFAULT_PATCH_SIZE)
        merge_size = vision_config.get('merge_size', vision_config.get('spatial_merge_size', DEFAULT_MERGE_SIZE))
        temporal_patch_size = vision_config.get('temporal_patch_size', DEFAULT_TEMPORAL_PATCH_SIZE)
        target_width = vision_config.get('target_width', vision_config.get('image_width', DEFAULT_TARGET_WIDTH))
        target_height = vision_config.get('target_height', vision_config.get('image_height', DEFAULT_TARGET_HEIGHT))
        image_mean = np.array(
            vision_config.get('image_mean', vision_config.get('normalize_mean', DEFAULT_IMAGE_MEAN)),
            dtype=np.float32
        )
        image_std = np.array(
            vision_config.get('image_std', vision_config.get('normalize_std', DEFAULT_IMAGE_STD)),
            dtype=np.float32
        )
    else:
        patch_size = DEFAULT_PATCH_SIZE
        merge_size = DEFAULT_MERGE_SIZE
        temporal_patch_size = DEFAULT_TEMPORAL_PATCH_SIZE
        target_width = DEFAULT_TARGET_WIDTH
        target_height = DEFAULT_TARGET_HEIGHT
        image_mean = DEFAULT_IMAGE_MEAN
        image_std = DEFAULT_IMAGE_STD

    factor = patch_size * merge_size

    logger.debug("[PREPROCESSING] Starting image preprocessing")

    # Apply EXIF orientation correction
    img = ImageOps.exif_transpose(img)
    logger.debug("[PREPROCESSING] Applied EXIF orientation correction")

    # Ensure RGB mode
    if img.mode != "RGB":
        logger.debug(f"[PREPROCESSING] Converting from {img.mode} to RGB mode")
        img = img.convert("RGB")
    else:
        logger.debug("[PREPROCESSING] Image already in RGB mode")

    # Get original dimensions
    original_width, original_height = img.size
    logger.debug(f"[PREPROCESSING] Original image dimensions: {original_width}x{original_height}")

    # Downscale to target size as per model specifications
    logger.debug(f"[PREPROCESSING] Downscaling to target size: {target_width}x{target_height}")

    # Letterbox to target size: scale to fit preserving aspect ratio, then center-pad
    scale = min(target_width / original_width, target_height / original_height)
    fit_w = int(original_width * scale)
    fit_h = int(original_height * scale)
    logger.debug(f"[PREPROCESSING] Letterbox: scale={scale:.4f}, fit={fit_w}x{fit_h}, target={target_width}x{target_height}")

    img = img.resize((fit_w, fit_h), resample=Image.BICUBIC)

    # Create padded image
    padded = Image.new("RGB", (target_width, target_height), (0, 0, 0))
    offset_x = (target_width - fit_w) // 2
    offset_y = (target_height - fit_h) // 2
    padded.paste(img, (offset_x, offset_y))
    img = padded
    logger.debug(f"[PREPROCESSING] Letterbox padding applied: offset=({offset_x},{offset_y})")

    w, h = img.size
    logger.debug(f"[PREPROCESSING] Image after letterbox: {w}x{h}")

    # Floor dimensions to multiples of factor
    new_w = (w // factor) * factor
    new_h = (h // factor) * factor
    logger.debug(f"[PREPROCESSING] Flooring to factor({factor}) multiples: {w}x{h} → {new_w}x{new_h}")

    if new_w < factor or new_h < factor:
        logger.error(f"[PREPROCESSING] Image too small after flooring: ({new_w},{new_h})")
        raise ValueError(
            f"Image too small after flooring: ({new_w},{new_h}); "
            f"needs >= {factor} on each side. Target size: ({w},{h})"
        )

    # Bicubic resize to floored dimensions
    logger.debug(f"[PREPROCESSING] Final bicubic resize to floored dimensions: {new_w}x{new_h}")
    img = img.resize((new_w, new_h), resample=Image.BICUBIC)

    # Convert to numpy float32 in [0,1]
    logger.debug("[PREPROCESSING] Converting image to numpy float32 array")
    arr = np.asarray(img, dtype=np.uint8).astype(np.float32) / 255.0
    logger.debug(f"[PREPROCESSING] Array shape after conversion: {arr.shape} (H, W, C)")

    # Normalize with model-specific mean/std
    logger.debug("[PREPROCESSING] Applying normalization (mean/std)")
    arr = (arr - image_mean.reshape(1, 1, 3)) / image_std.reshape(1, 1, 3)  # H, W, C
    logger.debug("[PREPROCESSING] Normalization completed")

    # Transpose to C, H, W
    logger.debug("[PREPROCESSING] Transposing from HWC to CHW format")
    chw = arr.transpose(2, 0, 1)  # C, H, W
    C, H, W = chw.shape
    logger.debug(f"[PREPROCESSING] CHW shape: {C}x{H}x{W}")

    # Build "frames" and pad to temporal_patch_size by repeating the last frame
    logger.debug("[PREPROCESSING] Building temporal frames")
    frames = np.expand_dims(chw, axis=0)  # (1, C, H, W)
    logger.debug(f"[PREPROCESSING] Initial frames shape: {frames.shape}")

    if frames.shape[0] % temporal_patch_size != 0:
        repeats = temporal_patch_size - (frames.shape[0] % temporal_patch_size)
        logger.debug(f"[PREPROCESSING] Padding frames by repeating {repeats} times for temporal_patch_size={temporal_patch_size}")
        frames = np.concatenate([frames, np.repeat(frames[-1][np.newaxis], repeats, axis=0)], axis=0)

    logger.debug(f"[PREPROCESSING] Final frames shape: {frames.shape}")

    grid_t = frames.shape[0] // temporal_patch_size  # 1
    grid_h, grid_w = H // patch_size, W // patch_size
    logger.debug(f"[PREPROCESSING] Grid dimensions: grid_t={grid_t}, grid_h={grid_h}, grid_w={grid_w}")
    logger.debug(f"[PREPROCESSING] Total patches: {grid_t * grid_h * grid_w}")

    # Reshape & transpose
    logger.debug("[PREPROCESSING] Reshaping into patches...")
    patches = frames.reshape(
        grid_t,                  # 1
        temporal_patch_size,     # 2
        C,
        grid_h // merge_size,    # H grouped by merge_size
        merge_size,
        patch_size,
        grid_w // merge_size,    # W grouped by merge_size
        merge_size,
        patch_size,
    )
    logger.debug(f"[PREPROCESSING] Patches shape after reshape: {patches.shape}")

    logger.debug("[PREPROCESSING] Transposing patches...")
    patches = patches.transpose(0, 3, 6, 4, 7, 2, 1, 5, 8)
    logger.debug(f"[PREPROCESSING] Patches shape after transpose: {patches.shape}")

    # Flatten to (L, D)
    # L = grid_t * grid_h * grid_w - number of patches
    # D = C * temporal_patch_size * patch_size * patch_size - floats per patch
    logger.debug("[PREPROCESSING] Flattening to (L, D) format...")
    flat = patches.reshape(
        grid_t * grid_h * grid_w,
        C * temporal_patch_size * patch_size * patch_size
    )
    logger.debug(f"[PREPROCESSING] Final tensor shape: {flat.shape} (L={flat.shape[0]}, D={flat.shape[1]})")

    # Ensure float32 little-endian
    logger.debug("[PREPROCESSING] Ensuring float32 little-endian format")
    if flat.dtype != np.float32:
        logger.debug(f"[PREPROCESSING] Converting from {flat.dtype} to float32")
        flat = flat.astype(np.float32, copy=False)
    if flat.dtype.byteorder not in ("<", "=", "|"):
        logger.debug("[PREPROCESSING] Converting to little-endian byte order")
        flat = flat.byteswap().newbyteorder("<")

    buffer_size = len(np.ascontiguousarray(flat).tobytes())
    logger.debug(f"[PREPROCESSING] Final buffer size: {buffer_size:,} bytes")
    logger.debug("[PREPROCESSING] Image preprocessing completed successfully")

    return PreprocessedImage(
        pixel_values=flat,
        grid_t=grid_t,
        grid_h=grid_h,
        grid_w=grid_w,
        original_width=original_width,
        original_height=original_height,
        resized_width=new_w,
        resized_height=new_h
    )

def preprocess_from_raw_bytes(raw_bytes: bytes, width: int, height: int,
                              mode: str = "RGB", vision_config: dict = None) -> PreprocessedImage:
    """
    Preprocess an image from raw pixel bytes.

    Args:
        raw_bytes: Raw pixel data bytes
        width: Image width
        height: Image height
        mode: PIL image mode (default: "RGB")
        vision_config: Optional model-specific preprocessing parameters

    Returns:
        PreprocessedImage object
    """
    # Reconstruct PIL Image from raw bytes
    img = Image.frombytes(mode, (width, height), raw_bytes)
    return preprocess_image(img, vision_config=vision_config)

def preprocess_from_decoded(decoded_image, vision_config: dict = None) -> PreprocessedImage:
    """
    Preprocess an image from a DecodedImage object.

    Args:
        decoded_image: DecodedImage object from image_validator module
        vision_config: Optional model-specific preprocessing parameters

    Returns:
        PreprocessedImage object
    """
    return preprocess_from_raw_bytes(
        decoded_image.raw_bytes,
        decoded_image.width,
        decoded_image.height,
        decoded_image.mode,
        vision_config=vision_config
    )

def preprocess_from_messages(messages: List) -> List[PreprocessedImage]:
    """
    Extract and preprocess all images from user messages.

    Args:
        messages: List of message dictionaries containing image content

    Returns:
        List of PreprocessedImage objects

    Raises:
        ValueError: If image validation or preprocessing fails
    """
    from .image_validator import extract_and_decode_images

    # Extract and decode all images from messages
    decoded_images = extract_and_decode_images(messages)

    # Preprocess each decoded image
    preprocessed = []
    for decoded_img in decoded_images:
        try:
            preprocessed_img = preprocess_from_decoded(decoded_img)
            preprocessed.append(preprocessed_img)
        except Exception as e:
            raise ValueError(f"Failed to preprocess image: {str(e)}")

    return preprocessed

def preprocess_from_data_url(data_url: str) -> PreprocessedImage:
    """
    Preprocess an image directly from a base64 data URL.

    Args:
        data_url: Base64-encoded data URL (e.g., "data:image/jpeg;base64,...")

    Returns:
        PreprocessedImage object
    """
    from .image_validator import decode_image

    decoded_img = decode_image(data_url)
    return preprocess_from_decoded(decoded_img)

def get_image_token_count(width: int, height: int) -> int:
    """
    Calculate the number of tokens an image will produce after preprocessing.

    Note: All images are now letterboxed to 512x342 before processing,
    so the input width/height parameters are ignored.

    Args:
        width: Image width in pixels (ignored - kept for API compatibility)
        height: Image height in pixels (ignored - kept for API compatibility)

    Returns:
        Number of tokens (patches) the image will produce (always consistent)
    """
    # All images are now standardized to 512x342, then floored to FACTOR multiples
    TARGET_WIDTH = 512
    TARGET_HEIGHT = 342

    # Floor to FACTOR multiples
    new_w = (TARGET_WIDTH // FACTOR) * FACTOR  # 504
    new_h = (TARGET_HEIGHT // FACTOR) * FACTOR  # 336

    # Calculate grid dimensions
    grid_h = new_h // PATCH_SIZE  # 336 // 14 = 24
    grid_w = new_w // PATCH_SIZE  # 504 // 14 = 36
    grid_t = 1  # Single frame for static images

    return grid_t * grid_h * grid_w  # 1 * 24 * 36 = 864 tokens per image
