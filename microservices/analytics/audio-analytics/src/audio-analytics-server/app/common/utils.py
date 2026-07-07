# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import base64
from typing import Optional


def base64_to_bytes(encoded_str: str) -> Optional[bytes]:
    """
    Decode a base64 encoded string to bytes.
    
    Args:
        encoded_str: Base64 encoded string
        
    Returns:
        Decoded bytes or None if decoding fails
    """
    try:
        byte_data = base64.b64decode(encoded_str)
        return byte_data
    except Exception as e:
        print(f"Error decoding base64: {e}")
        return None


def bytes_to_base64(byte_data: bytes) -> str:
    """
    Encode bytes to a base64 string.
    
    Args:
        byte_data: Bytes to encode
        
    Returns:
        Base64 encoded string
    """
    return base64.b64encode(byte_data).decode('utf-8')


def is_json_message(message: str) -> bool:
    """
    Check if a message is JSON (vs binary data).
    
    Args:
        message: Message string
        
    Returns:
        True if message appears to be JSON
    """
    if not message:
        return False
    
    message = message.strip()
    return message.startswith('{') or message.startswith('[')
