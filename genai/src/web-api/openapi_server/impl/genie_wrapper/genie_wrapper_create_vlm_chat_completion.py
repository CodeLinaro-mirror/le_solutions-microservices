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
from openapi_server.session.conversation_utils import ConversationUtils
from openapi_server.utils.image_cache import get_image_cache
from openapi_server.utils.image_validator import decode_image
from openapi_server.utils.image_preprocessor import preprocess_from_decoded

# Initialize logger
LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)


class GenieWrapperCreateVLMChatCompletion:
    """Enhanced VLM chat completion handler with caching and subprocess execution."""

    @staticmethod
    def build_vlm_prompt_for_turn(messages: List, text_prompt: str, has_image: bool, model_id: str) -> str:
        """
        Build complete VLM prompt for a single turn with proper chat template.
        Supports custom system prompts from the messages array.

        Args:
            messages: Full message history (to extract system prompt if present)
            text_prompt: The user's text from current message
            has_image: Whether this turn includes an image
            model_id: Model identifier for template retrieval

        Returns:
            Complete formatted prompt ready for C++ layer
        """
        from openapi_server.utils.common_utils import CommonUtils

        # Prepare messages for prompt builder
        prompt_messages = []

        # Extract system message if present
        for msg in messages:
            if isinstance(msg, dict):
                role = msg.get('role')
                content = msg.get('content')
            else:
                role = getattr(msg, 'role', None)
                content = getattr(msg, 'content', None)

            if role == 'system':
                prompt_messages.append({'role': 'system', 'content': content})
                break

        # Add user message
        prompt_messages.append({
            'role': 'user',
            'content': text_prompt
        })

        # Use unified builder
        complete_prompt = CommonUtils.build_chat_prompt(
            model_id=model_id,
            messages=prompt_messages,
            include_assistant_prefix=True,
            has_vision=has_image
        )

        logger.info(f"Built VLM prompt: {len(complete_prompt)} chars")
        return complete_prompt

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
                from openapi_server.utils.image_preprocessor import preprocess_image
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
    def build_chat_completion_response(content: str, model: str, session_id: str) -> CreateChatCompletionResponse:
        """
        Build a ChatCompletionResponse from VLM content.

        Args:
            content: VLM response content
            model: Model name
            session_id: Session/chat completion ID to use in response

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
            id=session_id,  # Use session_id instead of generating new ID
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
        Main entry point for VLM chat completion with named pipe IPC.

        Args:
            request_data: The chat completion request
            raw_json: Optional raw JSON to bypass Pydantic issues (should include 'session_id')

        Returns:
            Chat completion response, streaming response, or error
        """
        logger.info(f"=== VLM Chat Completion Request (Streaming={getattr(request_data, 'stream', False)}) ===")

        pipe_path = None
        temp_file_path = None
        process = None

        try:
            # Extract session_id from raw_json (passed from VisionConversationEvent)
            # This ensures VLM responses use the same ID as the chat completion session
            session_id = raw_json.get('session_id') if raw_json else None
            if not session_id:
                # Fallback: generate a chat completion ID if not provided
                session_id = f"chat-{uuid.uuid4()}"
                logger.warning(f"No session_id provided in raw_json, generated: {session_id}")
            else:
                logger.info(f"Using session_id from raw_json: {session_id}")

            # Calculate conversation hash for caching (session identification)
            conversation_hash = ConversationUtils.calculate_conversation_hash(
                request_data.messages,
                exclude_last_pair=False
            )
            logger.info(f"Conversation hash (session ID): {conversation_hash}")

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
            image_cache = get_image_cache()

            # Determine image source and handle caching
            preprocessed_image_bytes = None
            subprocess_image_input = None

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
                    preprocessed_image_bytes = cached_image_bytes
                    # Set fallback
                    cached_base64 = base64.b64encode(cached_image_bytes).decode('utf-8')
                    subprocess_image_input = f"data:image/preprocessed;base64,{cached_base64}"

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

            # Create named pipe
            pipe_path = tempfile.mktemp(suffix='.fifo', prefix='vlm_')
            os.mkfifo(pipe_path)
            logger.info(f"Created named pipe: {pipe_path}")

            # Build COMPLETE formatted prompt (includes system prompt extraction)
            complete_prompt = GenieWrapperCreateVLMChatCompletion.build_vlm_prompt_for_turn(
                messages=request_data.messages,
                text_prompt=text_prompt,
                has_image=(preprocessed_image_bytes is not None or subprocess_image_input is not None),
                model_id=request_data.model
            )

            # Build subprocess command with pipe
            # Note: We pass complete_prompt as text_prompt
            cmd, temp_file_path = GenieWrapperCreateVLMChatCompletion.build_subprocess_command(
                request_data.model, subprocess_image_input, complete_prompt, request_data,
                preprocessed_image_bytes=preprocessed_image_bytes
            )

            # Add pipe to command
            cmd.extend(["--output-pipe", pipe_path])

            logger.info(f"Executing VLM subprocess with pipe: {' '.join(cmd[:5])}... (truncated)")

            # Launch subprocess (non-blocking)
            process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

            # Handle streaming vs non-streaming
            if getattr(request_data, "stream", False):
                return await GenieWrapperCreateVLMChatCompletion._handle_streaming_response(
                    pipe_path, process, request_data, conversation_hash, temp_file_path
                )
            else:
                return await GenieWrapperCreateVLMChatCompletion._handle_non_streaming_response(
                    pipe_path, process, request_data, conversation_hash, temp_file_path
                )

        except Exception as e:
            logger.error(f"Unexpected error in VLM chat completion: {e}", exc_info=True)
            # Cleanup
            GenieWrapperCreateVLMChatCompletion._cleanup_resources(pipe_path, temp_file_path, process)
            return Error(
                code="500",
                message=f"VLM request failed: {str(e)}",
                param=Parameters.INTERNAL_TYPE,
                type=Parameters.INTERNAL_TYPE
            )

    @staticmethod
    async def _handle_streaming_response(pipe_path: str, process: subprocess.Popen,
                                        request_data: CreateChatCompletionRequest,
                                        session_id: str, temp_file_path: Optional[str]) -> StreamingResponse:
        """Handle streaming response via named pipe."""
        created = int(time.time())
        model = request_data.model

        async def stream_generator():
            try:
                with open(pipe_path, 'r') as pipe:
                    # First chunk: role
                    first_chunk = {
                        "id": session_id,
                        "object": "chat.completion.chunk",
                        "created": created,
                        "model": model,
                        "choices": [{
                            "index": 0,
                            "delta": {"role": "assistant", "content": None},
                            "finish_reason": None,
                            "logprobs": None
                        }]
                    }
                    yield f"data: {json.dumps(first_chunk)}\n\n"

                    # Read tokens from pipe
                    for line in pipe:
                        data = json.loads(line.strip())

                        if data['type'] == 'token':
                            # Content delta chunk
                            content_chunk = {
                                "id": session_id,
                                "object": "chat.completion.chunk",
                                "created": created,
                                "model": model,
                                "choices": [{
                                    "index": 0,
                                    "delta": {"content": data['content']},
                                    "finish_reason": None,
                                    "logprobs": None
                                }]
                            }
                            yield f"data: {json.dumps(content_chunk)}\n\n"

                        elif data['type'] == 'done':
                            # Final chunk
                            final_chunk = {
                                "id": session_id,
                                "object": "chat.completion.chunk",
                                "created": created,
                                "model": model,
                                "choices": [{
                                    "index": 0,
                                    "delta": {},
                                    "finish_reason": data['finish_reason'],
                                    "logprobs": None
                                }]
                            }
                            yield f"data: {json.dumps(final_chunk)}\n\n"
                            yield "data: [DONE]\n\n"
                            break

                        elif data['type'] == 'error':
                            raise Exception(data['message'])

            except Exception as e:
                logger.error(f"Streaming error: {e}")
                error_chunk = {
                    "id": session_id,
                    "object": "chat.completion.chunk",
                    "created": created,
                    "model": model,
                    "choices": [{
                        "index": 0,
                        "delta": {},
                        "finish_reason": "error",
                        "logprobs": None
                    }]
                }
                yield f"data: {json.dumps(error_chunk)}\n\n"
                yield "data: [DONE]\n\n"
            finally:
                # Cleanup
                GenieWrapperCreateVLMChatCompletion._cleanup_resources(pipe_path, temp_file_path, process)

        # Set SSE headers
        response_headers = {
            "Cache-Control": "no-cache",
            "X-Accel-Buffering": "no"
        }

        return StreamingResponse(
            stream_generator(),
            media_type="text/event-stream",
            headers=response_headers
        )

    @staticmethod
    async def _handle_non_streaming_response(pipe_path: str, process: subprocess.Popen,
                                            request_data: CreateChatCompletionRequest,
                                            session_id: str, temp_file_path: Optional[str]) -> Union[CreateChatCompletionResponse, Error]:
        """Handle non-streaming response via named pipe."""
        accumulated_content = []
        finish_reason = "stop"

        try:
            with open(pipe_path, 'r') as pipe:
                for line in pipe:
                    data = json.loads(line.strip())

                    if data['type'] == 'token':
                        accumulated_content.append(data['content'])

                    elif data['type'] == 'done':
                        finish_reason = data['finish_reason']
                        break

                    elif data['type'] == 'error':
                        raise Exception(data['message'])

            # Build complete response
            full_content = ''.join(accumulated_content)

            message = ChatCompletionResponseMessage(
                role="assistant",
                content=full_content,
                refusal=None
            )

            choice = CreateChatCompletionResponseChoicesInner(
                finish_reason=finish_reason,
                index=0,
                message=message,
                logprobs=None
            )

            response = CreateChatCompletionResponse(
                id=session_id,
                object="chat.completion",
                created=int(time.time()),
                model=request_data.model,
                choices=[choice]
            )

            return response

        except Exception as e:
            logger.error(f"Non-streaming error: {e}")
            return Error(
                code="500",
                message=f"VLM request failed: {str(e)}",
                param=Parameters.INTERNAL_TYPE,
                type=Parameters.INTERNAL_TYPE
            )
        finally:
            # Cleanup
            GenieWrapperCreateVLMChatCompletion._cleanup_resources(pipe_path, temp_file_path, process)

    @staticmethod
    def _cleanup_resources(pipe_path: Optional[str], temp_file_path: Optional[str], process: Optional[subprocess.Popen]):
        """Clean up pipe, temp file, and process."""
        # Clean up pipe
        if pipe_path and os.path.exists(pipe_path):
            try:
                os.unlink(pipe_path)
                logger.info(f"Cleaned up pipe: {pipe_path}")
            except Exception as e:
                logger.warning(f"Failed to clean up pipe {pipe_path}: {e}")

        # Clean up temp file
        if temp_file_path and os.path.exists(temp_file_path):
            try:
                os.unlink(temp_file_path)
                logger.info(f"Cleaned up temp file: {temp_file_path}")
            except Exception as e:
                logger.warning(f"Failed to clean up temp file {temp_file_path}: {e}")

        # Wait for process
        if process:
            try:
                process.wait(timeout=5)
                logger.info(f"Process exited with code: {process.returncode}")
            except subprocess.TimeoutExpired:
                logger.warning("Process did not exit within timeout, terminating")
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    logger.warning("Process did not terminate, killing")
                    process.kill()
