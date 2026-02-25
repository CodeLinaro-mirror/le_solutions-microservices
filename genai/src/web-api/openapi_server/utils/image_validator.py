# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import base64
import io
from typing import List, Optional, Tuple
from PIL import Image
import requests
from urllib.parse import urlparse

SUPPORTED_IMAGE_FORMATS = {'PNG', 'JPEG', 'WEBP', 'GIF'}
MAX_IMAGE_SIZE_MB = 50
MAX_IMAGES_PER_REQUEST = 500
DEFAULT_DOWNLOAD_TIMEOUT = 30  # seconds

class DecodedImage:
    def __init__(self, raw_bytes: bytes, format: str, width: int, height: int, mode: str, size_bytes: int):
        self.raw_bytes = raw_bytes
        self.format = format
        self.width = width
        self.height = height
        self.mode = mode
        self.size_bytes = size_bytes

def is_url(image_url: str) -> bool:
    """
    Check if image_url is an HTTP/HTTPS URL (vs base64 data URL).

    Args:
        image_url: The image URL string

    Returns:
        True if HTTP/HTTPS URL, False if data URL
    """
    return image_url.startswith(('http://', 'https://'))

def download_image_from_url(url: str, timeout: int = DEFAULT_DOWNLOAD_TIMEOUT) -> bytes:
    """
    Download image from HTTP/HTTPS URL.

    Args:
        url: HTTP/HTTPS URL to image
        timeout: Request timeout in seconds

    Returns:
        Image bytes

    Raises:
        ValueError: If download fails or invalid image
    """
    try:
        # Validate URL scheme
        parsed = urlparse(url)
        if parsed.scheme not in ('http', 'https'):
            raise ValueError(f"Invalid URL scheme '{parsed.scheme}'. Only http:// and https:// are supported.")

        # Download with timeout and streaming - add User-Agent to avoid 403 Forbidden
        headers = {
            'User-Agent': 'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36',
            'Accept': 'image/webp,image/apng,image/*,*/*;q=0.8',
            'Accept-Language': 'en-US,en;q=0.9',
            'Connection': 'keep-alive'
        }
        response = requests.get(url, timeout=timeout, stream=True, headers=headers)
        response.raise_for_status()

        # Validate content type
        content_type = response.headers.get('content-type', '').lower()
        if content_type and not content_type.startswith('image/'):
            raise ValueError(f"URL does not point to an image. Content-Type: {content_type}")

        # Check size limit before downloading full content
        content_length = response.headers.get('content-length')
        if content_length:
            size_mb = int(content_length) / (1024 * 1024)
            if size_mb > MAX_IMAGE_SIZE_MB:
                raise ValueError(f"Image exceeds size limit. Size: {size_mb:.2f}MB, Limit: {MAX_IMAGE_SIZE_MB}MB")

        # Read image data
        image_data = response.content

        # Verify size after download
        if len(image_data) > MAX_IMAGE_SIZE_MB * 1024 * 1024:
            size_mb = len(image_data) / (1024 * 1024)
            raise ValueError(f"Image exceeds size limit. Size: {size_mb:.2f}MB, Limit: {MAX_IMAGE_SIZE_MB}MB")

        if len(image_data) == 0:
            raise ValueError("Downloaded image is empty")

        return image_data

    except requests.Timeout:
        raise ValueError(f"Timeout downloading image from URL (timeout: {timeout}s): {url}")
    except requests.HTTPError as e:
        status_code = e.response.status_code if e.response else 'unknown'
        raise ValueError(f"HTTP error {status_code} downloading image from URL: {url}")
    except requests.RequestException as e:
        raise ValueError(f"Network error downloading image from URL: {str(e)}")
    except Exception as e:
        raise ValueError(f"Error downloading image from URL: {str(e)}")

def parse_data_url(data_url: str) -> Tuple[str, bytes]:
    """
    Parses a data URL and returns (mime_type, base64_bytes).
    """
    if not data_url.startswith("data:"):
        raise ValueError("Image URL must start with 'data:'")
    mime_marker = ";base64,"
    i = data_url.find(mime_marker)
    if i == -1:
        raise ValueError("Data URL missing ';base64,' marker")
    mime_type = data_url[5:i]
    base64_str = data_url[i + len(mime_marker):]
    try:
        decoded = base64.b64decode(base64_str)
    except Exception:
        raise ValueError("Invalid base64 payload in image URL")
    return mime_type, decoded

def decode_image(image_url: str) -> DecodedImage:
    """
    Fully decode an image from either a data URL or HTTP/HTTPS URL.
    Performs all validation as well.

    Args:
        image_url: Either base64 data URL (data:image/...) or HTTP/HTTPS URL

    Returns:
        DecodedImage object with decoded image data and metadata

    Raises:
        ValueError: If image cannot be downloaded, decoded, or validated
    """
    # Determine if URL or data URL and get compressed bytes
    if is_url(image_url):
        # Download from HTTP/HTTPS URL
        compressed = download_image_from_url(image_url)
    else:
        # Parse base64 data URL
        mime_type, compressed = parse_data_url(image_url)

    # Validate size
    if len(compressed) > MAX_IMAGE_SIZE_MB * 1024 * 1024:
        raise ValueError("Image exceeds size limit (50MB)")

    try:
        with Image.open(io.BytesIO(compressed)) as img:
            img_format = img.format
            if img_format not in SUPPORTED_IMAGE_FORMATS:
                raise ValueError(f"Unsupported image format: {img_format}")
            # Only allow non-animated GIFs
            if img_format == 'GIF' and getattr(img, "is_animated", False):
                raise ValueError("Animated GIFs are not supported")
            # Convert to RGB for standard LLM processing
            if img.mode not in ("RGB", "RGBA"):
                img = img.convert("RGB")
            raw_bytes = img.tobytes()
            return DecodedImage(
                raw_bytes=raw_bytes,
                format=img_format,
                width=img.width,
                height=img.height,
                mode=img.mode,
                size_bytes=len(compressed)
            )
    except Exception as e:
        raise ValueError(f"Failed to decode image: {str(e)}")

def has_image_content(messages: List) -> bool:
    """
    Check if any message contains image content.

    Args:
        messages: List of message dictionaries (raw JSON)

    Returns:
        True if any message contains images, False otherwise
    """
    import logging
    logger = logging.getLogger(__name__)

    logger.debug("=" * 60)
    logger.debug("FIX: Using raw JSON dict approach to detect images")
    logger.debug("This bypasses the broken Pydantic OneOf deserialization")
    logger.debug("=" * 60)
    logger.debug(f"[has_image_content] Checking {len(messages)} messages for image content")

    for msg_idx, m in enumerate(messages):
        # Work with raw dict
        if not isinstance(m, dict):
            logger.debug(f"[has_image_content] Message {msg_idx}: not a dict, type = {type(m)}")
            continue

        content = m.get('content')
        # Truncate content preview to avoid logging large responses
        if isinstance(content, str):
            content_preview = content[:100] + "..." if len(content) > 100 else content
        else:
            content_preview = f'list with {len(content) if isinstance(content, list) else 0} items'
        logger.debug(f"[has_image_content] Message {msg_idx}: content type = {type(content)}, content = {content_preview}")

        if content is None:
            logger.debug(f"[has_image_content] Message {msg_idx}: content is None, skipping")
            continue

        # Content can be a string or a list
        if isinstance(content, str):
            logger.debug(f"[has_image_content] Message {msg_idx}: content is string, skipping")
            continue

        if isinstance(content, list):
            logger.debug(f"[has_image_content] Message {msg_idx}: content is list with {len(content)} items")
            for item_idx, item in enumerate(content):
                if not isinstance(item, dict):
                    logger.debug(f"[has_image_content] Message {msg_idx}, Item {item_idx}: not a dict, type = {type(item)}")
                    continue

                item_type = item.get('type')
                logger.debug(f"[has_image_content] Message {msg_idx}, Item {item_idx}: type = {item_type}")

                if item_type == "image_url":
                    logger.info(f"[has_image_content] ✓ IMAGE DETECTED in message {msg_idx}, item {item_idx}")
                    return True

    logger.info("[has_image_content] ✗ NO IMAGES DETECTED in any message")
    return False

def extract_and_decode_images(messages: List) -> List[DecodedImage]:
    """
    Extract and decode all images from messages.

    Args:
        messages: List of message dictionaries or Pydantic models

    Returns:
        List of DecodedImage objects

    Raises:
        ValueError: If too many images or image processing fails
    """
    import logging
    logger = logging.getLogger(__name__)

    images = []
    for msg_idx, m in enumerate(messages):
        # Handle both dict (raw JSON) and Pydantic models
        if isinstance(m, dict):
            content = m.get('content')
        else:
            content = getattr(m, 'content', None)

        if content is None:
            continue

        # Content can be a string or a list
        if isinstance(content, str):
            continue

        if not isinstance(content, list):
            continue

        for item_idx, item in enumerate(content):
            # Handle both dict (raw JSON) and Pydantic models
            if isinstance(item, dict):
                item_type = item.get('type')
                if item_type == "image_url":
                    # Extract URL from dictionary
                    image_url_obj = item.get("image_url", {})
                    url_data = image_url_obj.get("url") if isinstance(image_url_obj, dict) else None
                    if url_data:
                        logger.info(f"Extracting image from message {msg_idx}, item {item_idx}: {url_data[:100]}...")
                        img = decode_image(url_data)
                        images.append(img)
                        if len(images) > MAX_IMAGES_PER_REQUEST:
                            raise ValueError("Too many images in one request (limit 500)")
            else:
                # Pydantic model - try to extract data
                item_type = getattr(item, 'type', None)
                if item_type == "image_url":
                    # Extract URL from Pydantic model
                    image_url_obj = getattr(item, 'image_url', None)
                    if image_url_obj:
                        if hasattr(image_url_obj, 'url'):
                            url_data = image_url_obj.url
                        elif isinstance(image_url_obj, dict):
                            url_data = image_url_obj.get('url')
                        else:
                            continue

                        if url_data:
                            logger.info(f"Extracting image from message {msg_idx}, item {item_idx}: {url_data[:100]}...")
                            img = decode_image(url_data)
                            images.append(img)
                            if len(images) > MAX_IMAGES_PER_REQUEST:
                                raise ValueError("Too many images in one request (limit 500)")

    logger.info(f"Extracted and decoded {len(images)} images total")
    return images
