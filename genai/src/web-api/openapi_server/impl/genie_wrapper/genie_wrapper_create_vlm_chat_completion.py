# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
Integrated VLM Chat Completion Handler with Subprocess Architecture

This module provides VLM (Vision Language Model) request handling using
the subprocess architecture via VLMProcessManager, replacing direct CFFI calls.

Key features:
- Subprocess-based VLM execution via VLMProcessManager
- Process kept alive between requests for performance
- Automatic process restart on model/session changes
- Support for both URL and base64 encoded images
- Streaming support via SSE generator
- Log redirection from subprocess to parent
- Integration with existing chat_utils session management
"""

import os
import time
import uuid
import json
import base64
import asyncio
from typing import Union, Optional, Tuple, List, Dict, Any

from fastapi.responses import StreamingResponse
from openapi_server.models.create_chat_completion_request import CreateChatCompletionRequest
from openapi_server.models.create_chat_completion_response import CreateChatCompletionResponse
from openapi_server.models.chat_completion_response_message import ChatCompletionResponseMessage
from openapi_server.models.create_chat_completion_response_choices_inner import CreateChatCompletionResponseChoicesInner
from openapi_server.models.error import Error
from openapi_server.logger.logger_config import LoggerConfig
from openapi_server.impl.constant import Parameters
from openapi_server.session.conversation_utils import ConversationUtils
from openapi_server.utils.common_utils import CommonUtils
from openapi_server.managers.model_config_manager import ModelConfigManager
from openapi_server.managers.metrics_manager import MetricsManager
from openapi_server.impl.genie_wrapper.vlm_process_manager import VLMProcessManager

# Initialize logger
LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)


class GenieWrapperCreateVLMChatCompletionIntegrated:
    """Integrated VLM chat completion handler using subprocess architecture."""

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
            Complete formatted prompt ready for subprocess
        """
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
    async def preprocess_image_from_input(image_input: str, model_id: str = None) -> bytes:
        """
        Preprocess an image from URL or base64 input.

        Args:
            image_input: Image URL or base64 encoded string
            model_id: Optional model identifier for model-specific preprocessing

        Returns:
            Preprocessed image bytes

        Raises:
            Exception: If image processing fails
        """
        try:
            # Look up vision preprocessing config for the model
            vision_config = None
            if model_id:
                config_manager = ModelConfigManager()
                vision_config = config_manager.get_vision_preprocessing(model_id)

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
                preprocessed = preprocess_image(img, vision_config=vision_config)

            else:
                # URL-based image
                logger.info(f"Processing image from URL: {image_input}")
                from openapi_server.utils.image_validator import decode_image
                from openapi_server.utils.image_preprocessor import preprocess_from_decoded
                decoded_img = decode_image(image_input)
                preprocessed = preprocess_from_decoded(decoded_img, vision_config=vision_config)

            # Convert to bytes
            image_bytes = preprocessed.to_bytes()
            logger.info(f"Image preprocessed successfully: {len(image_bytes)} bytes")
            return image_bytes

        except Exception as e:
            logger.error(f"Failed to preprocess image: {e}")
            raise

    @staticmethod
    async def create_vlm_chat_completion(request_data: CreateChatCompletionRequest,
                                       raw_json: dict = None,
                                       completion_callback=None,
                                       event_id: Optional[str] = None,
                                       event_state: Optional[str] = None,
                                       event_object=None) -> Union[CreateChatCompletionResponse, StreamingResponse, Error]:
        """
        Main entry point for VLM chat completion using subprocess architecture.

        Args:
            request_data: The chat completion request
            raw_json: Optional raw JSON to bypass Pydantic issues (should include 'session_id')
            completion_callback: Optional async callback to trigger when streaming completes
            event_id: Optional event ID for callback
            event_state: Optional event state for callback
            event_object: Optional event object for proper completion before callback

        Returns:
            Chat completion response, streaming response, or error
        """
        logger.info(f"=== VLM Chat Completion Request (Subprocess, Streaming={getattr(request_data, 'stream', False)}) ===")

        try:
            # Extract session_id from raw_json
            session_id = raw_json.get('session_id') if raw_json else None
            if not session_id:
                session_id = f"chat-{uuid.uuid4()}"
                logger.warning(f"No session_id provided in raw_json, generated: {session_id}")
            else:
                logger.info(f"Using session_id from raw_json: {session_id}")

            # Calculate conversation hash for session tracking
            conversation_hash = ConversationUtils.calculate_conversation_hash(
                request_data.messages,
                exclude_last_pair=False
            )
            logger.info(f"Conversation hash (session ID): {conversation_hash}")

            # Extract image and text from current message
            image_input, text_prompt = GenieWrapperCreateVLMChatCompletionIntegrated.extract_image_and_text_from_messages(
                request_data.messages, raw_json
            )

            if not text_prompt:
                return Error(
                    code="400",
                    message="No text prompt found in the request",
                    param=Parameters.INTERNAL_TYPE,
                    type=Parameters.INTERNAL_TYPE
                )

            # Check for image in current message
            if not image_input:
                return Error(
                    code="400",
                    message="No image found in current message. Each VLM request must include an image.",
                    param=Parameters.INTERNAL_TYPE,
                    type=Parameters.INTERNAL_TYPE
                )

            # Process the image from current request and measure preprocessing time
            logger.info("Processing image from current request...")
            preprocessing_start = time.time()
            try:
                preprocessed_image_bytes = await GenieWrapperCreateVLMChatCompletionIntegrated.preprocess_image_from_input(image_input, model_id=request_data.model)
                preprocessing_time_ms = (time.time() - preprocessing_start) * 1000
                logger.info(f"Image preprocessed successfully: {len(preprocessed_image_bytes)} bytes in {preprocessing_time_ms:.2f}ms")

            except Exception as e:
                logger.error(f"Failed to preprocess image: {e}")
                return Error(
                    code="400",
                    message=f"Failed to process image: {str(e)}",
                    param=Parameters.INTERNAL_TYPE,
                    type=Parameters.INTERNAL_TYPE
                )

            # Build complete formatted prompt
            complete_prompt = GenieWrapperCreateVLMChatCompletionIntegrated.build_vlm_prompt_for_turn(
                messages=request_data.messages,
                text_prompt=text_prompt,
                has_image=True,
                model_id=request_data.model
            )

            # Get VLM process manager
            vlm_manager = VLMProcessManager.get_instance()

            # Handle streaming vs non-streaming
            streaming = getattr(request_data, "stream", False)

            if streaming:
                return await GenieWrapperCreateVLMChatCompletionIntegrated._handle_streaming_response(
                    vlm_manager, request_data, complete_prompt, preprocessed_image_bytes,
                    session_id, preprocessing_time_ms, completion_callback, event_id, event_state, event_object
                )
            else:
                return await GenieWrapperCreateVLMChatCompletionIntegrated._handle_non_streaming_response(
                    vlm_manager, request_data, complete_prompt, preprocessed_image_bytes,
                    session_id, preprocessing_time_ms, completion_callback, event_id, event_state, event_object
                )

        except Exception as e:
            logger.error(f"Unexpected error in VLM chat completion: {e}", exc_info=True)
            return Error(
                code="500",
                message=f"VLM request failed: {str(e)}",
                param=Parameters.INTERNAL_TYPE,
                type=Parameters.INTERNAL_TYPE
            )

    @staticmethod
    async def _handle_streaming_response(
        vlm_manager: VLMProcessManager,
        request_data: CreateChatCompletionRequest,
        prompt: str,
        image_bytes: bytes,
        session_id: str,
        preprocessing_time_ms: float = 0.0,
        completion_callback=None,
        event_id: Optional[str] = None,
        event_state: Optional[str] = None,
        event_object=None
    ) -> StreamingResponse:
        """Handle streaming response using VLMProcessManager."""
        created = int(time.time())
        model = request_data.model

        async def stream_generator():
            stream_start_time = time.time()
            ttft_timestamp = None
            last_token_timestamp = None
            inter_token_latencies = []
            completion_tokens = 0

            try:
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

                # Stream tokens from VLM process
                async for token in vlm_manager.execute_request(
                    event_id=event_id or f"vlm-{uuid.uuid4()}",
                    session_id=session_id,
                    model=model,
                    prompt=prompt,
                    image_bytes=image_bytes,
                    streaming=True,
                    max_tokens=request_data.max_completion_tokens or 300,
                    temperature=request_data.temperature or 0.7,
                    top_p=request_data.top_p or 0.9,
                    top_k=getattr(request_data, "top_k", None),
                    presence_penalty=request_data.presence_penalty or 0.0,
                    frequency_penalty=request_data.frequency_penalty or 0.0
                ):
                    now = time.time()
                    # Capture TTFT on first token
                    if ttft_timestamp is None:
                        ttft_timestamp = now
                    # Capture inter-token latency for subsequent tokens
                    if last_token_timestamp is not None:
                        inter_token_latencies.append((now - last_token_timestamp) * 1000)
                    last_token_timestamp = now
                    completion_tokens += 1

                    # Content chunk
                    content_chunk = {
                        "id": session_id,
                        "object": "chat.completion.chunk",
                        "created": created,
                        "model": model,
                        "choices": [{
                            "index": 0,
                            "delta": {"content": token},
                            "finish_reason": None,
                            "logprobs": None
                        }]
                    }
                    yield f"data: {json.dumps(content_chunk)}\n\n"

                # Final chunk with stop reason
                final_chunk = {
                    "id": session_id,
                    "object": "chat.completion.chunk",
                    "created": created,
                    "model": model,
                    "choices": [{
                        "index": 0,
                        "delta": {},
                        "finish_reason": "stop",
                        "logprobs": None
                    }]
                }
                yield f"data: {json.dumps(final_chunk)}\n\n"
                yield "data: [DONE]\n\n"

                logger.info(f"VLM Event {event_id}: Streaming completed")

            except Exception as e:
                # Check if this error is due to cancellation
                if event_object and getattr(event_object, 'is_cancelled', False):
                    logger.info(f"VLM Event {event_id}: Stream terminated due to cancellation")
                elif "closed file" in str(e).lower() or isinstance(e, (EOFError, BrokenPipeError)):
                    logger.info(f"VLM Event {event_id}: Stream terminated (likely due to cancellation)")
                else:
                    logger.error(f"VLM Event {event_id}: Streaming error: {e}", exc_info=True)
                    # Record failure for health monitoring (not for cancellations)
                    try:
                        MetricsManager.get_instance().record_inference_failure(model)
                    except Exception:
                        pass
                    from openapi_server.impl.constant import GenieErrorMappings
                    error_msg = str(e)
                    layman_msg = GenieErrorMappings.get_layman_message(error_msg)
                    if layman_msg:
                        final_msg = layman_msg
                    else:
                        # Strip internal technical prefixes before showing to user
                        clean_msg = error_msg
                        for prefix in ("LLM subprocess error: ", "VLM subprocess error: "):
                            if clean_msg.startswith(prefix):
                                clean_msg = clean_msg[len(prefix):]
                                break
                        final_msg = clean_msg

                    error_payload = {
                        "error": {
                            "message": final_msg,
                            "type": "server_error",
                            "param": None,
                            "code": 500
                        }
                    }
                    yield f"data: {json.dumps(error_payload)}\n\n"
                yield "data: [DONE]\n\n"
            finally:
                # Submit metrics to MetricsManager
                try:
                    if completion_tokens > 0:
                        total_pipeline_latency_ms = (time.time() - stream_start_time) * 1000
                        ttft_ms = (ttft_timestamp - stream_start_time) * 1000 if ttft_timestamp else None
                        avg_stream_latency_ms = (
                            sum(inter_token_latencies) / len(inter_token_latencies)
                            if inter_token_latencies else None
                        )
                        MetricsManager.get_instance().record_inference_metrics(
                            model_id=model,
                            total_pipeline_latency_ms=total_pipeline_latency_ms,
                            tokens_generated=completion_tokens,
                            ttft_ms=ttft_ms,
                            avg_stream_latency_ms=avg_stream_latency_ms,
                            preprocessing_time_ms=preprocessing_time_ms if preprocessing_time_ms > 0 else None,
                        )
                        logger.debug(
                            f"VLM Event {event_id}: metrics — "
                            f"Preprocessing={preprocessing_time_ms:.1f}ms, "
                            f"TTFT={ttft_ms:.1f}ms, "
                            f"StreamLatency={avg_stream_latency_ms:.1f}ms, "
                            f"Total={total_pipeline_latency_ms:.1f}ms, "
                            f"Tokens={completion_tokens}"
                        )
                except Exception as metrics_err:
                    logger.error(f"VLM Event {event_id}: Failed to record metrics: {metrics_err}")

                # Complete/cancel event before callback
                if event_object:
                    try:
                        from openapi_server.events.conversation_event import EventState
                        if event_object.state == EventState.ACTIVE:
                            if getattr(event_object, 'is_cancelled', False):
                                logger.info(f"VLM Event {event_id}: Stream ended due to cancellation")
                                event_object.cancel_turn()
                            else:
                                logger.info(f"VLM Event {event_id}: Completing event from streaming generator")
                                event_object.complete_turn()
                                event_object.calculate_event_hash()
                                if hasattr(event_object, 'session') and event_object.session:
                                    event_object.session.complete_current_event()
                                logger.info(f"VLM Event {event_id}: Event completed successfully")
                    except Exception as e:
                        logger.error(f"VLM Event {event_id}: Error completing/cancelling event: {e}", exc_info=True)

                # Trigger completion callback
                if completion_callback:
                    logger.info(f"VLM Event {event_id}: Triggering completion callback")
                    try:
                        await completion_callback(event_id, event_state)
                    except Exception as e:
                        logger.error(f"VLM Event {event_id}: Error in streaming completion callback: {e}", exc_info=True)

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
    async def _handle_non_streaming_response(
        vlm_manager: VLMProcessManager,
        request_data: CreateChatCompletionRequest,
        prompt: str,
        image_bytes: bytes,
        session_id: str,
        preprocessing_time_ms: float = 0.0,
        completion_callback=None,
        event_id: Optional[str] = None,
        event_state: Optional[str] = None,
        event_object=None
    ) -> Union[CreateChatCompletionResponse, Error]:
        """Handle non-streaming response using VLMProcessManager."""
        try:
            # Accumulate tokens from VLM process
            accumulated_content = []
            non_stream_start = time.time()

            async for token in vlm_manager.execute_request(
                event_id=event_id or f"vlm-{uuid.uuid4()}",
                session_id=session_id,
                model=request_data.model,
                prompt=prompt,
                image_bytes=image_bytes,
                streaming=False,
                max_tokens=request_data.max_completion_tokens or 300,
                temperature=request_data.temperature or 0.7,
                top_p=request_data.top_p or 0.9,
                top_k=getattr(request_data, "top_k", None),
                presence_penalty=request_data.presence_penalty or 0.0,
                frequency_penalty=request_data.frequency_penalty or 0.0
            ):
                accumulated_content.append(token)

            # Record non-streaming metrics (TPS approximated as total_tokens / total_time)
            try:
                if accumulated_content:
                    MetricsManager.get_instance().record_inference_metrics(
                        model_id=request_data.model,
                        total_pipeline_latency_ms=(time.time() - non_stream_start) * 1000,
                        tokens_generated=len(accumulated_content),
                        preprocessing_time_ms=preprocessing_time_ms if preprocessing_time_ms > 0 else None,
                    )
            except Exception as metrics_err:
                logger.error(f"VLM Event {event_id}: Failed to record non-streaming metrics: {metrics_err}")

            # Build response
            full_content = ''.join(accumulated_content)

            message = ChatCompletionResponseMessage(
                role="assistant",
                content=full_content,
                refusal=None
            )

            choice = CreateChatCompletionResponseChoicesInner(
                finish_reason="stop",
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

            logger.info(f"VLM Event {event_id}: Non-streaming completed, {len(full_content)} chars")

            return response

        except Exception as e:
            logger.error(f"VLM non-streaming error: {e}", exc_info=True)
            return Error(
                code="500",
                message=f"VLM request failed: {str(e)}",
                param=Parameters.INTERNAL_TYPE,
                type=Parameters.INTERNAL_TYPE
            )
        finally:
            # Complete/cancel event before callback
            if event_object:
                try:
                    from openapi_server.events.conversation_event import EventState
                    if event_object.state == EventState.ACTIVE:
                        if getattr(event_object, 'is_cancelled', False):
                            logger.info(f"VLM Event {event_id}: Non-streaming cancelled")
                            event_object.cancel_turn()
                        else:
                            logger.info(f"VLM Event {event_id}: Completing event from non-streaming response")
                            event_object.complete_turn()
                            event_object.calculate_event_hash()
                            if hasattr(event_object, 'session') and event_object.session:
                                event_object.session.complete_current_event()
                            logger.info(f"VLM Event {event_id}: Event completed successfully")
                except Exception as e:
                    logger.error(f"VLM Event {event_id}: Error completing/cancelling event: {e}", exc_info=True)

            # Trigger completion callback
            if completion_callback:
                logger.info(f"VLM Event {event_id}: Triggering completion callback")
                try:
                    await completion_callback(event_id, event_state)
                except Exception as e:
                    logger.error(f"VLM Event {event_id}: Error in non-streaming completion callback: {e}", exc_info=True)
