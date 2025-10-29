# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
Enhanced VLM Chat Completion Handler with Image Caching

This module provides the main entry point for VLM (Vision Language Model) requests
with intelligent image caching capabilities. It replaces the previous complex
CFFI thread-based approach with a simpler subprocess-based execution model.

Key features:
- Image caching using conversation hash as session identifier
- Support for both URL and base64 encoded images
- Subprocess-based VLM execution for isolation and simplicity
- Streaming support via SSE generator simulation
- Fallback logic for follow-up questions without images
- Integration with existing chat_utils session management
"""

import subprocess
import os
import time
import uuid
import json
import base64
import asyncio
import tempfile
from typing import Union, Optional, Tuple, List, Dict, Any

from fastapi.responses import StreamingResponse
from openapi_server.models.create_chat_completion_request import CreateChatCompletionRequest
from openapi_server.models.create_chat_completion_response import CreateChatCompletionResponse
from openapi_server.models.chat_completion_response_message import ChatCompletionResponseMessage
from openapi_server.models.create_chat_completion_response_choices_inner import CreateChatCompletionResponseChoicesInner
from openapi_server.models.error import Error
from openapi_server.logger.logger_config import LoggerConfig
from openapi_server.impl.constant import LLMServiceKeys, Parameters
from openapi_server.impl.genie_wrapper.chat.utils.chat_utils import ChatQueryUtils
from openapi_server.impl.genie_wrapper.utils.vlm_image_cache import get_vlm_image_cache
from openapi_server.impl.genie_wrapper.utils.image_validator import decode_image
from openapi_server.impl.genie_wrapper.utils.image_preprocessor import preprocess_from_decoded
from openapi_server.impl.genie_wrapper.chat.vlm_sse_generator import create_vlm_sse_generator

# Initialize logger
LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)


class GenieWrapperCreateVLMChatCompletion:
    """Enhanced VLM chat completion handler with caching and subprocess execution."""

    @staticmethod
    def extract_image_and_text_from_messages(messages: List, raw_json: dict = None) -> Tuple[Optional[str], str]:
        """
        Extract image URL/base64 and text content from the last message.

        Args:
            messages: List of message objects
            raw_json: Optional raw JSON data to bypass Pydantic issues

        Returns:
            Tuple of (image_input, text_content) where image_input can be URL or base64
        """
        if not messages:
            return None, ""

        # Use raw_json if available to avoid Pydantic OneOf issues
        if raw_json and 'messages' in raw_json and raw_json['messages']:
            last_message = raw_json['messages'][-1]
            content = last_message.get('content', [])
        else:
            last_message = messages[-1]
            content = getattr(last_message, 'content', [])

        text_content = ""
        image_input = None

        # Handle different content formats
        if isinstance(content, str):
            # Simple text message
            text_content = content
        elif isinstance(content, list):
            # Multimodal content with text and images
            for item in content:
                if isinstance(item, dict):
                    # Raw JSON format
                    if item.get('type') == 'text':
                        text_content = item.get('text', '')
                    elif item.get('type') == 'image_url':
                        image_url_data = item.get('image_url', {})
                        if isinstance(image_url_data, dict):
                            image_input = image_url_data.get('url', '')
                        else:
                            image_input = str(image_url_data)
                elif hasattr(item, 'type'):
                    # Pydantic object format
                    if item.type == 'text' and hasattr(item, 'text'):
                        text_content = getattr(item, 'text', '')
                    elif item.type == 'image_url' and hasattr(item, 'image_url'):
                        image_url_obj = getattr(item, 'image_url', None)
                        if hasattr(image_url_obj, 'url'):
                            image_input = image_url_obj.url
                        elif isinstance(image_url_obj, dict):
                            image_input = image_url_obj.get('url', '')

        logger.info(f"Extracted from last message - Text: {len(text_content)} chars, Image: {'Yes' if image_input else 'No'}")
        return image_input, text_content

    @staticmethod
    def search_all_messages_for_image(messages: List, raw_json: dict = None) -> Optional[str]:
        """
        Search all messages in the conversation for an image URL or base64.

        Args:
            messages: List of message objects
            raw_json: Optional raw JSON data

        Returns:
            Image URL/base64 string or None if not found
        """
        logger.info("Searching all messages for images...")

        # Use raw_json if available
        if raw_json and 'messages' in raw_json:
            messages_data = raw_json['messages']
        else:
            messages_data = messages

        for i, msg in enumerate(messages_data):
            if isinstance(msg, dict):
                # Raw JSON format
                content = msg.get('content', [])
            else:
                # Pydantic object format
                content = getattr(msg, 'content', [])

            if isinstance(content, list):
                for item in content:
                    if isinstance(item, dict):
                        if item.get('type') == 'image_url':
                            image_url_data = item.get('image_url', {})
                            if isinstance(image_url_data, dict):
                                url = image_url_data.get('url', '')
                            else:
                                url = str(image_url_data)
                            if url:
                                logger.info(f"Found image in message {i}: {url[:50]}...")
                                return url
                    elif hasattr(item, 'type') and item.type == 'image_url':
                        image_url_obj = getattr(item, 'image_url', None)
                        if hasattr(image_url_obj, 'url'):
                            url = image_url_obj.url
                        elif isinstance(image_url_obj, dict):
                            url = image_url_obj.get('url', '')
                        else:
                            url = str(image_url_obj)
                        if url:
                            logger.info(f"Found image in message {i}: {url[:50]}...")
                            return url

        logger.info("No images found in any messages")
        return None

    @staticmethod
    async def preprocess_image_from_input(image_input: str) -> bytes:
        """
        Preprocess an image from URL or base64 input.

        Args:
            image_input: Image URL or base64 encoded string

        Returns:
            Preprocessed image bytes

        Raises:
            Exception: If image processing fails
        """
        try:
            if image_input.startswith('data:image') or (not image_input.startswith('http') and len(image_input) > 100):
                # Likely base64 encoded image
                logger.info("Processing base64 encoded image")

                # Handle data URL format
                if image_input.startswith('data:image'):
                    if ',' in image_input:
                        base64_data = image_input.split(',', 1)[1]
                    else:
                        raise ValueError("Invalid data URL format")
                else:
                    base64_data = image_input

                # Decode base64
                image_data = base64.b64decode(base64_data)

                # Create PIL Image
                from PIL import Image
                import io
                img = Image.open(io.BytesIO(image_data))

                # Preprocess using existing pipeline
                from openapi_server.impl.genie_wrapper.utils.image_preprocessor import preprocess_image
                preprocessed = preprocess_image(img)

            else:
                # URL-based image
                logger.info(f"Processing image from URL: {image_input}")
                decoded_img = decode_image(image_input)
                preprocessed = preprocess_from_decoded(decoded_img)

            # Convert to bytes
            image_bytes = preprocessed.to_bytes()
            logger.info(f"Image preprocessed successfully: {len(image_bytes)} bytes")
            return image_bytes

        except Exception as e:
            logger.error(f"Failed to preprocess image: {e}")
            raise

    @staticmethod
    def build_subprocess_command(model: str, image_input: Optional[str], text_prompt: str,
                               request_data: CreateChatCompletionRequest,
                               preprocessed_image_bytes: Optional[bytes] = None) -> Tuple[List[str], Optional[str]]:
        """
        Build the subprocess command for VLM execution.

        Args:
            model: Model name
            image_input: Image URL or base64 string (can be None for cached images)
            text_prompt: Text prompt
            request_data: Request parameters
            preprocessed_image_bytes: Optional preprocessed image bytes to pass directly

        Returns:
            Tuple of (command_list, temp_file_path)
            temp_file_path will be None if no temp file was created
        """
        vlm_script_path = os.path.join("/root", "app", "site-packages", "vlm_standalone_test.py")

        cmd = [
            "python3",
            vlm_script_path,
            "--model", model,
            "--prompt", text_prompt,
            "--max-tokens", str(request_data.max_completion_tokens or 300),
            "--temperature", str(request_data.temperature or 0.7),
            "--top-p", str(request_data.top_p or 0.9),
            "--presence-penalty", str(request_data.presence_penalty or 0.0),
            "--frequency-penalty", str(request_data.frequency_penalty or 0.0)
        ]

        temp_file_path = None

        # Add image parameter - prefer preprocessed bytes over URL/base64
        if preprocessed_image_bytes:
            # Write preprocessed image bytes to a temporary file
            try:
                # Create a temporary file with a unique name
                with tempfile.NamedTemporaryFile(delete=False, suffix='.bin') as temp_file:
                    temp_file_path = temp_file.name
                    # Write raw bytes directly to the file
                    temp_file.write(preprocessed_image_bytes)

                # Pass the temp file path to the subprocess
                cmd.extend(["--image-file", temp_file_path])
                logger.info(f"Wrote {len(preprocessed_image_bytes)} bytes to temp file: {temp_file_path}")
            except Exception as e:
                logger.error(f"Failed to create temp file for image bytes: {e}")
                # Fall back to URL/base64 if temp file creation fails
                if image_input:
                    if image_input.startswith('http'):
                        cmd.extend(["--image-url", image_input])
                    else:
                        cmd.extend(["--image-base64", image_input])
        elif image_input:
            # No preprocessed bytes available, use URL/base64
            if image_input.startswith('http'):
                cmd.extend(["--image-url", image_input])
            else:
                cmd.extend(["--image-base64", image_input])

        # Add streaming flag if requested
        if getattr(request_data, "stream", False):
            cmd.append("--stream")

        return cmd, temp_file_path

    @staticmethod
    def parse_vlm_subprocess_output(stdout: str) -> str:
        """
        Parse VLM response from subprocess stdout using uniform markers.

        Args:
            stdout: Raw stdout from subprocess

        Returns:
            Extracted VLM response content
        """
        stdout_lines = stdout.splitlines()

        # Parse VLM response using new uniform markers
        capture_response = False
        response_lines = []

        for line in stdout_lines:
            stripped_line = line.strip()

            if "=== VLM_RESPONSE_START ===" in stripped_line:
                capture_response = True
                continue

            if "=== VLM_RESPONSE_END ===" in stripped_line:
                capture_response = False
                break

            if capture_response:
                # Extract content after logger prefix if present
                if " - INFO - " in stripped_line:
                    content = stripped_line.split(" - INFO - ", 1)[1]
                    response_lines.append(content)
                else:
                    # Handle cases where content might not have logger prefix
                    response_lines.append(stripped_line)

        extracted_content = "\n".join(response_lines).strip()
        logger.info(f"Extracted VLM response: {len(extracted_content)} characters")
        return extracted_content

    @staticmethod
    def build_chat_completion_response(content: str, model: str) -> CreateChatCompletionResponse:
        """
        Build a ChatCompletionResponse from VLM content.

        Args:
            content: VLM response content
            model: Model name

        Returns:
            CreateChatCompletionResponse object
        """
        message = ChatCompletionResponseMessage(
            role="assistant",
            content=content,
            refusal=LLMServiceKeys.REFUSE
        )

        choices_inner = CreateChatCompletionResponseChoicesInner(
            finish_reason="stop",
            index=0,
            message=message,
            logprobs=None
        )

        response = CreateChatCompletionResponse(
            id=f"vlm-{uuid.uuid4().hex[:24]}",
            object="chat.completion",
            created=int(time.time()),
            model=model,
            choices=[choices_inner]
        )

        return response

    @staticmethod
    async def create_vlm_chat_completion(request_data: CreateChatCompletionRequest,
                                       raw_json: dict = None) -> Union[CreateChatCompletionResponse, StreamingResponse, Error]:
        """
        Main entry point for VLM chat completion with caching support.

        Args:
            request_data: The chat completion request
            raw_json: Optional raw JSON to bypass Pydantic issues

        Returns:
            Chat completion response, streaming response, or error
        """
        logger.info(f"=== Enhanced VLM Chat Completion Request (Streaming={getattr(request_data, 'stream', False)}) ===")

        try:
            # Calculate conversation hash for caching (session identification)
            conversation_hash = ChatQueryUtils.calculate_conversation_hash(
                request_data.messages,
                exclude_last_pair=False
            )
            logger.info(f"Conversation hash for caching: {conversation_hash}")

            # Extract image and text from current message
            image_input, text_prompt = GenieWrapperCreateVLMChatCompletion.extract_image_and_text_from_messages(
                request_data.messages, raw_json
            )

            if not text_prompt:
                return Error(
                    code="400",
                    message="No text prompt found in the request",
                    param=Parameters.INTERNAL_TYPE,
                    type=Parameters.INTERNAL_TYPE
                )

            # Get image cache
            image_cache = get_vlm_image_cache()

            # Determine image source and handle caching
            if image_input:
                # New image provided in current message
                logger.info("New image provided, preprocessing and caching...")
                try:
                    image_bytes = await GenieWrapperCreateVLMChatCompletion.preprocess_image_from_input(image_input)

                    # Cache the preprocessed image if we have a conversation hash
                    if conversation_hash:
                        source_desc = image_input[:50] + "..." if len(image_input) > 50 else image_input
                        image_cache.set(conversation_hash, image_bytes, source_desc)

                    # Use the preprocessed image bytes directly
                    subprocess_image_input = image_input  # Keep original for fallback
                    preprocessed_image_bytes = image_bytes

                except Exception as e:
                    logger.error(f"Failed to preprocess new image: {e}")
                    return Error(
                        code="400",
                        message=f"Failed to process image: {str(e)}",
                        param=Parameters.INTERNAL_TYPE,
                        type=Parameters.INTERNAL_TYPE
                    )
            else:
                # No image in current message - try cache first
                cached_image_bytes = None
                if conversation_hash:
                    cached_image_bytes = image_cache.get(conversation_hash)

                if cached_image_bytes:
                    # Use cached preprocessed image
                    logger.info(f"Using cached image for conversation {conversation_hash}")

                    # Keep the cached bytes for direct use
                    # We'll also set a fallback image input just in case
                    cached_base64 = base64.b64encode(cached_image_bytes).decode('utf-8')
                    subprocess_image_input = f"data:image/preprocessed;base64,{cached_base64}"
                    # cached_image_bytes is already set and will be used directly

                else:
                    # No cached image - search all messages for image
                    logger.info("No cached image found, searching all messages...")
                    found_image = GenieWrapperCreateVLMChatCompletion.search_all_messages_for_image(
                        request_data.messages, raw_json
                    )

                    if found_image:
                        # Found image in conversation history
                        logger.info("Found image in conversation history, preprocessing...")
                        try:
                            image_bytes = await GenieWrapperCreateVLMChatCompletion.preprocess_image_from_input(found_image)

                            # Cache for future use
                            if conversation_hash:
                                source_desc = found_image[:50] + "..." if len(found_image) > 50 else found_image
                                image_cache.set(conversation_hash, image_bytes, source_desc)

                            # Use the preprocessed image bytes directly
                            subprocess_image_input = found_image  # Keep original for fallback
                            preprocessed_image_bytes = image_bytes

                        except Exception as e:
                            logger.error(f"Failed to preprocess found image: {e}")
                            return Error(
                                code="400",
                                message=f"Failed to process image from conversation: {str(e)}",
                                param=Parameters.INTERNAL_TYPE,
                                type=Parameters.INTERNAL_TYPE
                            )
                    else:
                        # No image found anywhere
                        return Error(
                            code="400",
                            message="No image found in current message, cache, or conversation history",
                            param=Parameters.INTERNAL_TYPE,
                            type=Parameters.INTERNAL_TYPE
                        )

            # Build subprocess command with preprocessed image bytes if available
            preprocessed_bytes = locals().get('preprocessed_image_bytes') or locals().get('cached_image_bytes')
            cmd, temp_file_path = GenieWrapperCreateVLMChatCompletion.build_subprocess_command(
                request_data.model, subprocess_image_input, text_prompt, request_data,
                preprocessed_image_bytes=preprocessed_bytes
            )

            logger.info(f"Executing VLM subprocess: {' '.join(cmd[:5])}... (truncated)")

            # Execute subprocess
            try:
                result = subprocess.run(cmd, capture_output=True, text=True, check=True, timeout=300)

                # Parse response from combined output (VLM logs go to stderr)
                combined_output = (result.stdout or "") + "\n" + (result.stderr or "")
                response_content = GenieWrapperCreateVLMChatCompletion.parse_vlm_subprocess_output(combined_output)

                # Clean up temp file if it was created
                if temp_file_path and os.path.exists(temp_file_path):
                    try:
                        os.unlink(temp_file_path)
                        logger.info(f"Cleaned up temp file: {temp_file_path}")
                    except Exception as e:
                        logger.warning(f"Failed to clean up temp file {temp_file_path}: {e}")

                if not response_content:
                    logger.error("No response content extracted from subprocess output")
                    return Error(
                        code="500",
                        message="VLM subprocess returned empty response",
                        param=Parameters.INTERNAL_TYPE,
                        type=Parameters.INTERNAL_TYPE
                    )

                # Handle streaming vs non-streaming response
                if getattr(request_data, "stream", False):
                    # Create streaming response using SSE generator
                    logger.info("Creating streaming response with SSE generator")

                    response_data = {
                        "content": response_content,
                        "role": "assistant",
                        "finish_reason": "stop",
                        "id": f"vlm-{uuid.uuid4().hex[:24]}",
                        "model": request_data.model,
                        "created": int(time.time()),
                        "object": "chat.completion"
                    }

                    sse_generator = create_vlm_sse_generator(response_data)
                    return StreamingResponse(
                        sse_generator.generate_sse_chunks(),
                        media_type="text/event-stream"
                    )
                else:
                    # Create non-streaming response
                    logger.info("Creating non-streaming response")
                    return GenieWrapperCreateVLMChatCompletion.build_chat_completion_response(
                        response_content, request_data.model
                    )

            except subprocess.TimeoutExpired:
                logger.error("VLM subprocess timed out")
                # Clean up temp file if it was created
                if temp_file_path and os.path.exists(temp_file_path):
                    try:
                        os.unlink(temp_file_path)
                        logger.info(f"Cleaned up temp file after timeout: {temp_file_path}")
                    except Exception as e:
                        logger.warning(f"Failed to clean up temp file {temp_file_path}: {e}")
                return Error(
                    code="500",
                    message="VLM request timed out",
                    param=Parameters.INTERNAL_TYPE,
                    type=Parameters.INTERNAL_TYPE
                )
            except subprocess.CalledProcessError as e:
                logger.error(f"VLM subprocess failed: {e}")
                logger.error(f"Subprocess stderr: {e.stderr}")
                # Clean up temp file if it was created
                if temp_file_path and os.path.exists(temp_file_path):
                    try:
                        os.unlink(temp_file_path)
                        logger.info(f"Cleaned up temp file after subprocess error: {temp_file_path}")
                    except Exception as cleanup_error:
                        logger.warning(f"Failed to clean up temp file {temp_file_path}: {cleanup_error}")
                return Error(
                    code="500",
                    message=f"VLM subprocess failed: {e.stderr or str(e)}",
                    param=Parameters.INTERNAL_TYPE,
                    type=Parameters.INTERNAL_TYPE
                )

        except Exception as e:
            logger.error(f"Unexpected error in VLM chat completion: {e}", exc_info=True)
            # Clean up temp file if it was created
            if 'temp_file_path' in locals() and temp_file_path and os.path.exists(temp_file_path):
                try:
                    os.unlink(temp_file_path)
                    logger.info(f"Cleaned up temp file after unexpected error: {temp_file_path}")
                except Exception as cleanup_error:
                    logger.warning(f"Failed to clean up temp file {temp_file_path}: {cleanup_error}")
            return Error(
                code="500",
                message=f"VLM request failed: {str(e)}",
                param=Parameters.INTERNAL_TYPE,
                type=Parameters.INTERNAL_TYPE
            )
