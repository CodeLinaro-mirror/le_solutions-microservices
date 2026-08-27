# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
VisionConversationEvent: Concrete implementation for VLM (Vision Language Model) events.
Handles one complete turn with image support via optimized direct CFFI execution with pipeline reuse.
"""

import asyncio
import time
from typing import Optional, Dict, Any
from fastapi.responses import StreamingResponse

from openapi_server.events.conversation_event import (
    ConversationEvent,
    EventType,
    EventState
)
from openapi_server.impl.genie_wrapper.genie_wrapper_create_vlm_chat_completion import GenieWrapperCreateVLMChatCompletionIntegrated
from openapi_server.session.token_counter import TokenCounter
from openapi_server.managers.model_config_manager import ModelConfigManager
from openapi_server.models.error import Error
from openapi_server.logger.logger_config import LoggerConfig

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)


class VisionConversationEvent(ConversationEvent):
    """
    Vision conversation event representing ONE complete turn with VLM.

    Key differences from TextConversationEvent:
    - No handle management (VLM runs in subprocess)
    - Supports images in messages
    - Delegates to existing VLM handler
    - References messages in session's shared history via message_indices
    """

    def __init__(self, session: 'ConversationSession', model_id: str):
        import uuid
        event_id = f"event-{uuid.uuid4().hex[:16]}"

        super().__init__(event_id, model_id, EventType.VISION, session)

        # Get model context size
        config_manager = ModelConfigManager()
        self.context_size = config_manager.get_context_size(model_id)

        # Default max completion tokens when the client does not specify.
        # Capped at 50% of context_size when the client requests a larger value.
        from openapi_server.impl.constant import DEFAULT_MAX_COMPLETION_TOKENS
        self.default_max_completion_tokens = DEFAULT_MAX_COMPLETION_TOKENS

        logger.info(f"Created VisionConversationEvent {event_id} for model {model_id} (context: {self.context_size})")

    async def execute_turn(self, request_data) -> dict:
        """
        Execute VLM turn by delegating to existing VLM handler.

        The VLM handler will:
        1. Extract images from session.messages
        2. Use cached preprocessed images if available
        3. Execute VLM inference via subprocess
        4. Return response (streaming or non-streaming)
        """
        try:
            # ── Pre-flight: check combined image + text token budget ──────────
            # The VLM pipeline accumulates image tokens (from the vision encoder)
            # and text tokens (from the text tokenizer) before submitting to the
            # LLM (text generator), which has a fixed context_size limit.
            # We must check: image_tokens + text_tokens + output_reserve ≤ context_size
            #
            # Image token count is deterministic: all images are letterboxed to
            # 512×342 before patching, producing exactly 864 tokens per image
            # (see image_preprocessor.get_image_token_count()).
            from openapi_server.utils.image_preprocessor import get_image_token_count
            from openapi_server.impl.constant import (
                MAX_COMPLETION_SAFETY_MARGIN,
                ErrorMessages,
                HttpStatusCodes,
                ERROR_CODE_CONTEXT_LENGTH_EXCEEDED,
            )
            from fastapi import HTTPException as _HTTPException

            # Build raw_messages first — this is exactly what gets sent to the VLM
            # subprocess. Since VLM is stateless per inference, raw_messages represents
            # the complete input for this turn and is the correct set to count tokens for.
            raw_messages = [
                msg for msg in self.session.messages
                if msg.get('role') != 'system'
            ]
            if self.session.system_prompt_content:
                raw_messages.insert(0, {
                    'role': 'system',
                    'content': self.session.system_prompt_content,
                })

            # Count images and text tokens from raw_messages (what the VLM will actually see)
            image_count = 0
            for msg in raw_messages:
                content = msg.get('content', '')
                if isinstance(content, list):
                    for item in content:
                        if isinstance(item, dict) and item.get('type') == 'image_url':
                            image_count += 1

            from openapi_server.impl.constant import IMAGE_TOKEN_ESTIMATION_RATIO
            tokens_per_image = get_image_token_count(0, 0)  # 864 raw patches (fixed)
            # Apply estimation ratio to account for preprocessing optimizations
            # that reduce actual tokens consumed vs raw patch count.
            # Default 0.7: 864 * 0.7 = 604 estimated tokens per image.
            image_tokens = int(image_count * tokens_per_image * IMAGE_TOKEN_ESTIMATION_RATIO)

            # Estimate text tokens from non-system raw messages
            text_tokens = sum(
                TokenCounter.estimate_tokens_for_multimodal_content(msg.get('content', ''))
                for msg in raw_messages
                if msg.get('role') != 'system'
            )

            # Use a fixed DEFAULT_MAX_COMPLETION_TOKENS reservation for the
            # pre-flight guard (same logic as _check_user_query_length in LLM).
            # This ensures the model always has a minimum breather space to
            # respond, regardless of what the client requested.
            output_reserve = self.default_max_completion_tokens
            total_input = image_tokens + text_tokens
            available = self.context_size - output_reserve - MAX_COMPLETION_SAFETY_MARGIN

            if total_input > available:
                raise _HTTPException(
                    status_code=HttpStatusCodes.BAD_REQUEST,
                    detail={
                        "message": ErrorMessages.VLM_QUERY_TOO_LONG.format(
                            total_tokens=total_input,
                            image_tokens=image_tokens,
                            text_tokens=text_tokens,
                            available=available,
                            context_size=self.context_size,
                            output_tokens=output_reserve,
                        ),
                        "type": "invalid_request_error",
                        "code": ERROR_CODE_CONTEXT_LENGTH_EXCEEDED,
                        "param": "messages",
                    },
                )

            # Cap max_completion_tokens at whatever budget remains after the
            # prompt is assembled.  This mirrors the silent-cap behaviour of
            # _resolve_max_completion_tokens() in TextConversationEvent so that
            # clients requesting more tokens than available simply get fewer
            # tokens rather than a 400 error.
            requested_max = getattr(request_data, 'max_completion_tokens', None)
            available_for_response = max(0, self.context_size - total_input - MAX_COMPLETION_SAFETY_MARGIN)
            if requested_max is not None and requested_max > available_for_response:
                logger.debug(
                    f"Event {self.event_id}: VLM max_completion_tokens={requested_max} "
                    f"exceeds available={available_for_response} — capping silently"
                )
                effective_max_completion = available_for_response
            else:
                effective_max_completion = requested_max

            # raw_messages already built above — pass to VLM handler

            raw_json = {
                'messages': raw_messages,
                'model': self.model_id,
                'stream': getattr(request_data, 'stream', False),
                'max_completion_tokens': effective_max_completion,
                'temperature': getattr(request_data, 'temperature', None),
                'top_p': getattr(request_data, 'top_p', None),
                'top_k': getattr(request_data, 'top_k', None),
                'presence_penalty': getattr(request_data, 'presence_penalty', None),
                'frequency_penalty': getattr(request_data, 'frequency_penalty', None),
                'session_id': self.session.session_id  # Pass session ID for response consistency
            }

            logger.info(f"Event {self.event_id}: Executing VLM turn with {len(self.session.messages)} messages in history")

            # Delegate to integrated VLM handler with subprocess architecture
            # Pass completion callback for streaming lock release
            # Pass event object so VLM handler can complete the event before triggering callback
            result = await GenieWrapperCreateVLMChatCompletionIntegrated.create_vlm_chat_completion(
                request_data,
                raw_json,
                completion_callback=self._completion_callback,
                event_id=self.event_id,
                event_state=self.state,
                event_object=self  # Pass event object for proper completion
            )

            # Handle different result types
            if isinstance(result, Error):
                raise Exception(result.message)

            # Check if it's a tuple (streaming with content)
            if isinstance(result, tuple):
                streaming_response, content = result
                self.assistant_message = content  # Store content for session

                logger.info(f"Event {self.event_id}: Returning streaming response with {len(content)} chars")

                return {
                    "response": streaming_response,
                    "finish_reason": "stop",
                    "needs_tool_response": False,
                    "turn_complete": True,
                    "is_streaming": True
                }

            # Check if it's a StreamingResponse (shouldn't happen now, but handle it)
            if isinstance(result, StreamingResponse):
                logger.warning(f"Event {self.event_id}: Got StreamingResponse without content tuple")
                return {
                    "response": result,  # StreamingResponse object
                    "finish_reason": "stop",
                    "needs_tool_response": False,
                    "turn_complete": True,
                    "is_streaming": True
                }

            # Non-streaming response
            content = result.choices[0].message.content
            self.assistant_message = content
            # Note: complete_turn() will be called by the handler after adding assistant message index
            # This ensures the event hash includes all messages in the turn

            logger.info(f"Event {self.event_id}: Turn completed, {len(content)} chars")

            return {
                "response": content,
                "finish_reason": "stop",
                "needs_tool_response": False,
                "turn_complete": True,
                "is_streaming": False
            }

        except asyncio.CancelledError:
            logger.info(f"Event {self.event_id}: VLM turn cancelled by client")
            self.is_cancelled = True
            raise
        except Exception as e:
            # Re-raise HTTPException so it propagates as the correct HTTP status code
            # (e.g. 400 for context_length_exceeded) rather than being swallowed and
            # returned as HTTP 500 by the outer handler.
            from fastapi import HTTPException as _FastAPIHTTPException
            if isinstance(e, _FastAPIHTTPException):
                raise

            logger.error(f"Event {self.event_id}: VLM execution failed: {e}")
            self.terminate_handle(force=True)
            self.fail_turn(e)
            return {
                "response": None,
                "finish_reason": "error",
                "needs_tool_response": False,
                "turn_complete": False,
                "error": {
                    "type": type(e).__name__,
                    "message": str(e)
                }
            }

    async def continue_with_tool_response(self, tool_response: str, request_data) -> dict:
        """
        VLM doesn't support tool calling yet.
        Raise NotImplementedError if called.
        """
        raise NotImplementedError("VLM does not support tool calling")

    # No-op handle management methods (VLM uses subprocess, not CFFI handles)

    def take_over_handle(self, previous_event: 'ConversationEvent'):
        """
        No-op: VLM doesn't use handles (subprocess execution).
        """
        logger.debug(f"Event {self.event_id}: take_over_handle called (no-op for VLM)")
        pass

    def create_new_handle(self):
        """
        No-op: VLM doesn't use handles (subprocess execution).
        """
        logger.debug(f"Event {self.event_id}: create_new_handle called (no-op for VLM)")
        pass

    def release_handle(self):
        """
        No-op: VLM doesn't use handles (subprocess execution).
        """
        logger.debug(f"Event {self.event_id}: release_handle called (no-op for VLM)")
        pass

    def terminate_handle(self, force: bool = False):
        """
        Terminate VLM subprocess.
        Shuts down the VLM process to free resources.
        """
        from openapi_server.impl.genie_wrapper.vlm_process_manager import VLMProcessManager
        logger.info(f"Event {self.event_id}: Terminating VLM subprocess for model {self.model_id} (force={force})")
        try:
            VLMProcessManager.get_instance().shutdown(force=force)
        except Exception as e:
            logger.error(f"Event {self.event_id}: Error terminating VLM subprocess: {e}")

    def _calculate_prompt_tokens(self) -> int:
        """Calculate prompt tokens including image costs for this turn."""
        tokens = 0

        # Get messages this event is responsible for (user messages)
        for idx in self.message_indices:
            if idx < len(self.session.messages):
                msg = self.session.messages[idx]
                if msg.get('role') == 'user':
                    # Add role formatting tokens (4 tokens per message)
                    tokens += 4
                    # Add content tokens (text + images)
                    content = msg.get('content', '')
                    tokens += TokenCounter.estimate_tokens_for_multimodal_content(content)

        logger.debug(f"Event {self.event_id}: Calculated {tokens} prompt tokens (including images)")
        return tokens

    def _calculate_completion_tokens(self) -> int:
        """Calculate completion tokens from VLM response."""
        tokens = 0

        # Assistant message tokens
        if self.assistant_message:
            tokens += TokenCounter.estimate_tokens(self.assistant_message)

        # VLM doesn't support tool calling yet, so no tool overhead

        logger.debug(f"Event {self.event_id}: Calculated {tokens} completion tokens")
        return tokens

    def complete_turn(self):
        """
        Override complete_turn to prevent automatic handle termination in ADHOC_MODE.

        VLM handles should stay cached and only be destroyed when:
        1. Switching to a different VLM model (handled in conversation_session.py)
        2. Explicit cleanup is requested

        This is different from LLM handles which are destroyed after each turn in ADHOC_MODE.
        """
        if self.state == EventState.ACTIVE:
            self.state = EventState.COMPLETED
            self.completed_at = time.time()

            # Calculate detailed token breakdown
            self.calculate_token_usage()

            # Calculate hash for this turn
            self.calculate_turn_hash()

            logger.info(f"Event {self.event_id}: Turn COMPLETED, "
                       f"prompt: {self.prompt_tokens}, "
                       f"completion: {self.completion_tokens}, "
                       f"total: {self.total_turn_tokens} tokens")

            # IMPORTANT: Do NOT terminate VLM handle in ADHOC_MODE
            # VLM handles are cached and reused across requests
            # They are only destroyed when switching to a different VLM model
            logger.debug(f"Event {self.event_id}: VLM handle kept alive for reuse (ADHOC_MODE)")

            # NOTE: Do NOT trigger the completion callback here.
            # The VLM handler's finally block calls it after the subprocess
            # sends its final signal, ensuring the DSP is truly idle.
