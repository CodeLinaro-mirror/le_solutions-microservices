# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
Event-based chat completion handler using the new ConversationEvent architecture.
Integrates with existing OpenAI-compatible API while using hash-based session management.
"""

import json
import time
import asyncio
from typing import Union, Dict, Any, List
from fastapi import HTTPException
from fastapi.responses import StreamingResponse

from openapi_server.models.create_chat_completion_request import CreateChatCompletionRequest
from openapi_server.models.create_chat_completion_response import CreateChatCompletionResponse
from openapi_server.models.create_chat_completion_response_choices_inner import (
    ChatCompletionResponseMessage,
    CreateChatCompletionResponseChoicesInner
)
from openapi_server.models.completion_usage import CompletionUsage
from openapi_server.models.error import Error
from openapi_server.managers.session_manager import SessionManager
from openapi_server.session.conversation_session import ConversationSession
from openapi_server.session.conversation_utils import ConversationUtils
from openapi_server.events.text_conversation_event import TextConversationEvent
from openapi_server.events.vision_conversation_event import VisionConversationEvent
from openapi_server.session.tool_handler import ToolHandler
from openapi_server.utils.image_validator import has_image_content
from openapi_server.managers.model_config_manager import ModelConfigManager
from openapi_server.impl.constant import (
    HttpStatusCodes,
    ErrorMessages,
    Parameters,
    LLMServiceKeys,
    TOOL_RESPONSE_TIMEOUT_SECONDS,
)
from openapi_server.logger.logger_config import LoggerConfig

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)


class EventBasedChatHandler:
    """
    Event-based chat completion handler using ConversationEvent architecture.
    Uses hash-based session lookup with stable chat completion IDs.
    """

    @staticmethod
    def extract_messages_from_request(request_data, raw_json) -> List[Dict]:
        """Extract messages from request, preferring raw_json for image support."""
        if raw_json and 'messages' in raw_json:
            return raw_json['messages']
        elif hasattr(request_data, 'messages'):
            # Convert Pydantic models to dicts
            messages = []
            for msg in request_data.messages:
                msg_dict = {'role': msg.role}
                if hasattr(msg, 'content') and msg.content:
                    msg_dict['content'] = msg.content
                if hasattr(msg, 'tool_calls') and msg.tool_calls:
                    msg_dict['tool_calls'] = msg.tool_calls
                messages.append(msg_dict)
            return messages
        return []

    @staticmethod
    def _remove_session_messages(session, message_indices: List[int]) -> None:
        """Remove the given message indices from the shared session history."""
        for idx in sorted(message_indices, reverse=True):
            if idx < len(session.messages):
                session.messages.pop(idx)

    @staticmethod
    def _rollback_new_turn_preflight(
        session,
        event,
        previous_model,
        previous_system_prompt_content,
        previous_system_prompt_tokens,
    ) -> None:
        """
        Roll back a newly-created event when validation fails before any new
        inference should become part of the session state.
        """
        EventBasedChatHandler._remove_session_messages(session, event.message_indices)
        if session.current_event is event:
            session.current_event = None
        session.current_model = previous_model
        session.system_prompt_content = previous_system_prompt_content
        session.system_prompt_tokens = previous_system_prompt_tokens

    @staticmethod
    def _rollback_tool_continuation_preflight(session, event, added_message_indices: List[int]) -> None:
        """
        Roll back tool messages appended for Trip 2 when validation fails before
        the continuation inference should be accepted.
        """
        EventBasedChatHandler._remove_session_messages(session, added_message_indices)
        removed_indices = set(added_message_indices)
        event.message_indices = [idx for idx in event.message_indices if idx not in removed_indices]
        event.tool_info = None
        event._tool_response_received = False

    @staticmethod
    async def handle_chat_completion(
        request_data: CreateChatCompletionRequest,
        raw_json: dict = None,
        session = None,
        completion_callback = None
    ) -> Union[StreamingResponse, CreateChatCompletionResponse]:
        """
        Main entry point for event-based chat completion with hash-based session lookup.

        Args:
            request_data: The chat completion request
            raw_json: Optional raw JSON to bypass Pydantic issues
            session: Pre-resolved ConversationSession (REQUIRED - provided by API layer)
            completion_callback: Optional callback to register on event before execution

        Returns:
            Chat completion response or streaming response
        """
        try:
            from openapi_server.impl.constant import ADHOC_MODE

            logger.info("=== EVENT-BASED CHAT COMPLETION (HASH-BASED) ===")
            if ADHOC_MODE:
                logger.info("⚠️  ADHOC_MODE ENABLED - Handles will be created/destroyed per conversation turn")

            # Session must be provided by API layer
            if session is None:
                raise HTTPException(
                    status_code=500,
                    detail="Internal error: session not provided by API layer"
                )

            # Extract messages
            messages = EventBasedChatHandler.extract_messages_from_request(request_data, raw_json)

            if not messages:
                raise HTTPException(400, "No messages provided")

            requested_model = request_data.model or "qwen2.5-7b"

            # Log whether images are detected
            if has_image_content(messages):
                logger.info("✓ IMAGE CONTENT DETECTED - Will use VisionConversationEvent")
            else:
                logger.info("✗ NO IMAGE CONTENT DETECTED - Will use TextConversationEvent")

            # Validate model
            config_manager = ModelConfigManager()
            if not config_manager.validate_model(requested_model):
                raise HTTPException(
                    status_code=HttpStatusCodes.BAD_REQUEST,
                    detail=f"Model '{requested_model}' not found"
                )

            # Get chat completion ID from session
            chat_completion_id = session.session_id

            # Determine if this is a new session (no messages yet)
            is_new = len(session.messages) == 0

            logger.info(f"Session: {chat_completion_id} (new={is_new}, existing_messages={len(session.messages)})")
            session_mgr = SessionManager.get_instance()
            is_tool_response_request = messages[-1].get('role') == 'tool'

            if is_tool_response_request:
                user_messages_for_hash = [msg for msg in messages if msg.get('role') == 'user']
                if user_messages_for_hash:
                    tool_call_hash = ConversationUtils.calculate_hash_for_specific_messages(user_messages_for_hash)
                    if session_mgr.is_timed_out_tool_call(tool_call_hash):
                        raise HTTPException(
                            status_code=HttpStatusCodes.REQUEST_TIMEOUT,
                            detail="Tool response timed out. Please retry the turn."
                        )

            # STEP 1: Check if we're continuing a tool calling event FIRST
            # This must be checked before calculating new_messages to handle tool continuations correctly
            current_event = session.get_current_event()
            has_active_tool_call = (
                current_event and
                current_event.is_active() and
                current_event._is_tool_calling
            )
            is_tool_continuation = has_active_tool_call and is_tool_response_request

            if current_event and is_tool_response_request:
                if current_event.tool_wait_timed_out or current_event.has_tool_response_timed_out():
                    raise HTTPException(
                        status_code=HttpStatusCodes.REQUEST_TIMEOUT,
                        detail="Tool response timed out. Please retry the turn."
                    )

            if is_tool_response_request and not is_tool_continuation:
                raise HTTPException(
                    status_code=HttpStatusCodes.REQUEST_TIMEOUT,
                    detail="No active tool call is waiting for a response (request timed out)."
                )

            if has_active_tool_call and not is_tool_response_request:
                raise HTTPException(
                    status_code=HttpStatusCodes.CONFLICT,
                    detail="Tool response required before sending the next user message."
                )

            # STEP 2: Determine new messages based on whether this is a tool continuation
            if is_tool_continuation:
                # Tool continuation - process all trailing tool messages
                trailing_tool_messages = []
                for msg in reversed(messages):
                    if msg.get('role') == 'tool':
                        trailing_tool_messages.append(msg)
                    else:
                        break

                new_messages = list(reversed(trailing_tool_messages))
                logger.info(f"Tool continuation detected - processing {len(new_messages)} trailing tool message(s)")
            else:
                # New turn - calculate new messages based on session state
                if is_new:
                    # New session - add all messages
                    new_messages = messages
                else:
                    # Existing session - only add new messages not in session history
                    existing_count = len(session.messages)
                    new_messages = messages[existing_count:]

            # Validate we have messages to process
            if not new_messages:
                # If a prior streaming request was interrupted, the session may still
                # have an active event with the same last user message recorded.
                # In that case, cancel the active event and retry with fresh messages.
                if session.current_event and session.current_event.is_active():
                    logger.warning(
                        f"No new messages for session {session.session_id} with active event; "
                        f"cancelling active event to allow retry"
                    )
                    if session.cancel_active_event():
                        existing_count = len(session.messages)
                        new_messages = messages[existing_count:]

                if not new_messages:
                    raise HTTPException(400, "No new messages to process")

            logger.info(f"Processing {len(new_messages)} new message(s)")

            if is_tool_continuation:
                logger.info("=== CONTINUING TOOL CALLING ===")

                if not current_event.is_active() or current_event.tool_wait_timed_out or current_event.has_tool_response_timed_out():
                    raise HTTPException(
                        status_code=HttpStatusCodes.REQUEST_TIMEOUT,
                        detail="Tool response timed out. Please retry the turn."
                    )

                current_event.request_signature = session_mgr.build_request_signature(request_data, raw_json, messages)

                tool_response_parts = []
                added_tool_message_indices = []
                for tool_msg in new_messages:
                    tool_msg_idx = session.add_message(tool_msg)
                    added_tool_message_indices.append(tool_msg_idx)
                    current_event.message_indices.append(tool_msg_idx)

                    tool_call_id = tool_msg.get('tool_call_id', 'unknown')
                    tool_content = tool_msg.get('content', '')

                    if tool_call_id and tool_call_id != 'unknown':
                        tool_response_parts.append(f"[tool_call_id={tool_call_id}]\n{tool_content}")
                    else:
                        tool_response_parts.append(str(tool_content))

                tool_response = "\n\n".join(tool_response_parts).strip()

                # Continue turn with tool response (now async)
                try:
                    result = await current_event.continue_with_tool_response(tool_response, request_data)
                except HTTPException:
                    EventBasedChatHandler._rollback_tool_continuation_preflight(
                        session,
                        current_event,
                        added_tool_message_indices,
                    )
                    raise

                if result['turn_complete']:
                    # Add assistant response to session
                    assistant_msg = {
                        'role': 'assistant',
                        'content': result['response']
                    }
                    assistant_idx = session.add_message(assistant_msg)
                    current_event.message_indices.append(assistant_idx)

                    # Update legacy fields
                    current_event.assistant_message = result['response']
                    current_event.replay_result = {
                        'response': result['response'],
                        'finish_reason': result['finish_reason'],
                    }

                    # Unregister from tool calling map since tool calling is complete
                    # Use ONLY user messages for hash (same as registration)
                    user_message_indices = [idx for idx in current_event.message_indices
                                           if session.messages[idx].get('role') == 'user']
                    user_messages = [session.messages[idx] for idx in user_message_indices]
                    event_hash = ConversationUtils.calculate_hash_for_specific_messages(user_messages)
                    session_mgr.unregister_tool_calling_event(event_hash)

                    # Complete the event state
                    current_event.complete_turn()

                    # Calculate event hash AFTER all message indices are added and event is fully complete
                    # This includes: user + assistant(tool_calls) + tool + assistant(final response)
                    current_event.calculate_event_hash()

                    session.complete_current_event()

                return EventBasedChatHandler._create_response(
                    result, current_event, request_data, chat_completion_id
                )

            else:
                logger.info("=== NEW TURN ===")

                # Create new event
                previous_model = session.current_model
                previous_system_prompt_content = session.system_prompt_content
                previous_system_prompt_tokens = session.system_prompt_tokens
                event = session.create_event(
                    model_id=requested_model,
                    new_messages=new_messages,
                    is_tool_continuation=False
                )

                # Update legacy user_message field
                user_msgs = [m for m in new_messages if m.get('role') == 'user']
                if user_msgs:
                    event.user_message = user_msgs[-1]['content']

                event.request_signature = session_mgr.build_request_signature(request_data, raw_json, messages)

                # CRITICAL: Register completion callback BEFORE execute_turn()
                # This ensures the callback is available when VLM/LLM handlers need it
                if completion_callback:
                    event.register_completion_callback(completion_callback)
                    logger.info(f"✓ Registered completion callback on event {event.event_id} BEFORE execution")

                # Handle tool instructions if tools provided
                if request_data.tools and len(request_data.tools) > 0:
                    logger.info(f"Function calling enabled with {len(request_data.tools)} tools")
                    modified_request = request_data.model_copy(deep=True)
                    modified_request.messages = ToolHandler.inject_tool_instructions(
                        modified_request.messages,
                        request_data.tools
                    )
                    request_data = modified_request

                # Execute turn (now async) with error handling
                try:
                    result = await event.execute_turn(request_data)
                except HTTPException:
                    EventBasedChatHandler._rollback_new_turn_preflight(
                        session,
                        event,
                        previous_model,
                        previous_system_prompt_content,
                        previous_system_prompt_tokens,
                    )
                    raise
                except RuntimeError as e:
                    # Handle model loading failures and resource unavailability
                    error_msg = str(e)
                    logger.error(f"Event execution failed: {error_msg}")

                    from openapi_server.impl.constant import GenieErrorMappings
                    status_code = GenieErrorMappings.get_http_status_code(
                        error_msg,
                        default_status=HttpStatusCodes.INTERNAL_SERVER_ERROR,
                    )
                    layman_msg = GenieErrorMappings.get_layman_message(error_msg)

                    raise HTTPException(
                        status_code=status_code,
                        detail=layman_msg if layman_msg else error_msg
                    )

                # Handle streaming response
                if result.get('is_streaming', False):
                    # For streaming, return the StreamingResponse directly
                    return result['response']

                if result['finish_reason'] == 'tool_calls':
                    # Tool calling - add assistant message with tool_calls
                    assistant_msg = {
                        'role': 'assistant',
                        'content': None,
                        'tool_calls': result['response']
                    }
                    assistant_idx = session.add_message(assistant_msg)
                    event.message_indices.append(assistant_idx)

                    # Register this session in the tool calling map for fallback lookup
                    # Use ONLY user messages for stable hash during tool calling
                    user_message_indices = [idx for idx in event.message_indices
                                           if session.messages[idx].get('role') == 'user']
                    user_messages = [session.messages[idx] for idx in user_message_indices]
                    event_hash = ConversationUtils.calculate_hash_for_specific_messages(user_messages)
                    session_mgr.register_tool_calling_event(event_hash, session.session_id)
                    event.start_tool_response_timeout(TOOL_RESPONSE_TIMEOUT_SECONDS)

                elif result['turn_complete']:
                    # Regular response - add assistant message
                    assistant_msg = {
                        'role': 'assistant',
                        'content': result['response']
                    }
                    assistant_idx = session.add_message(assistant_msg)
                    event.message_indices.append(assistant_idx)

                    # Update legacy field
                    event.assistant_message = result['response']
                    event.replay_result = {
                        'response': result['response'],
                        'finish_reason': result['finish_reason'],
                    }

                    # Complete the event AFTER adding assistant message
                    event.complete_turn()

                    # Calculate event hash AFTER all message indices are added
                    # This ensures the hash includes all messages in the turn (user + assistant)
                    event.calculate_event_hash()

                    session.complete_current_event()

                return EventBasedChatHandler._create_response(
                    result, event, request_data, chat_completion_id
                )

        except HTTPException:
            raise
        except Exception as e:
            logger.error(f"Unexpected error in event-based chat handler: {e}", exc_info=True)
            from openapi_server.impl.constant import GenieErrorMappings
            error_msg = str(e)
            status_code = GenieErrorMappings.get_http_status_code(
                error_msg,
                default_status=HttpStatusCodes.INTERNAL_SERVER_ERROR,
            )
            layman_msg = GenieErrorMappings.get_layman_message(error_msg)
            raise HTTPException(
                status_code=status_code,
                detail=layman_msg if layman_msg else f"Internal server error: {error_msg}"
            )

    @staticmethod
    def _create_response(
        result: dict,
        event: Union[TextConversationEvent, VisionConversationEvent],
        request_data: CreateChatCompletionRequest,
        chat_completion_id: str
    ) -> Union[StreamingResponse, CreateChatCompletionResponse]:
        """
        Create OpenAI-compatible response from event result.

        Args:
            result: Result from event execution
            event: The conversation event
            request_data: Original request data
            chat_completion_id: The stable chat completion ID

        Returns:
            OpenAI-compatible response
        """
        if result.get('error'):
            from openapi_server.impl.constant import GenieErrorMappings
            error_msg = result['error']['message']
            status_code = GenieErrorMappings.get_http_status_code(
                error_msg,
                default_status=HttpStatusCodes.INTERNAL_SERVER_ERROR,
            )
            layman_msg = GenieErrorMappings.get_layman_message(error_msg)
            # Error response
            raise HTTPException(
                status_code=status_code,
                detail=layman_msg if layman_msg else error_msg
            )

        if result['finish_reason'] == 'tool_calls':
            # Tool calls response
            tool_calls = result['response']

            if request_data.stream:
                # Streaming tool calls response
                return EventBasedChatHandler._create_streaming_tool_response(
                    tool_calls, event, chat_completion_id
                )
            else:
                # Non-streaming tool calls response
                message = ChatCompletionResponseMessage(
                    role="assistant",
                    content=None,
                    refusal=LLMServiceKeys.REFUSE,
                    tool_calls=tool_calls
                )

                choice = CreateChatCompletionResponseChoicesInner(
                    finish_reason="tool_calls",
                    index=0,
                    message=message,
                    logprobs=None
                )

                # Create usage statistics for tool calls
                usage = CompletionUsage(
                    prompt_tokens=event.prompt_tokens,
                    completion_tokens=event.completion_tokens,
                    total_tokens=event.total_turn_tokens
                )

                return CreateChatCompletionResponse(
                    id=chat_completion_id,
                    object="chat.completion",
                    created=int(time.time()),
                    model=event.model_id,
                    choices=[choice],
                    usage=usage
                )

        else:
            # Regular response
            content = result['response']

            if request_data.stream:
                # Streaming regular response
                return EventBasedChatHandler._create_streaming_response(
                    content, event, chat_completion_id
                )
            else:
                # Non-streaming regular response
                message = ChatCompletionResponseMessage(
                    role="assistant",
                    content=content,
                    refusal=LLMServiceKeys.REFUSE,
                    tool_calls=None
                )

                choice = CreateChatCompletionResponseChoicesInner(
                    finish_reason="stop",
                    index=0,
                    message=message,
                    logprobs=None
                )

                # Create usage statistics for regular response
                usage = CompletionUsage(
                    prompt_tokens=event.prompt_tokens,
                    completion_tokens=event.completion_tokens,
                    total_tokens=event.total_turn_tokens
                )

                return CreateChatCompletionResponse(
                    id=chat_completion_id,
                    object="chat.completion",
                    created=int(time.time()),
                    model=event.model_id,
                    choices=[choice],
                    usage=usage
                )

    @staticmethod
    def _create_streaming_response(
        content: str,
        event: Union[TextConversationEvent, VisionConversationEvent],
        chat_completion_id: str
    ) -> StreamingResponse:
        """Create streaming response for regular content."""
        async def stream_generator():
            # Send role first
            chunk = {
                "id": chat_completion_id,
                "object": "chat.completion.chunk",
                "created": int(time.time()),
                "model": event.model_id,
                "choices": [{
                    "delta": {"role": "assistant"},
                    "index": 0,
                    "finish_reason": None
                }]
            }
            yield f"data: {json.dumps(chunk)}\n\n"

            # Send content in chunks
            words = content.split()
            for i, word in enumerate(words):
                chunk = {
                    "id": chat_completion_id,
                    "object": "chat.completion.chunk",
                    "created": int(time.time()),
                    "model": event.model_id,
                    "choices": [{
                        "delta": {"content": word + " "},
                        "index": 0,
                        "finish_reason": None
                    }]
                }
                yield f"data: {json.dumps(chunk)}\n\n"
                await asyncio.sleep(0.01)  # Small delay for streaming effect

            # Send final chunk
            chunk = {
                "id": chat_completion_id,
                "object": "chat.completion.chunk",
                "created": int(time.time()),
                "model": event.model_id,
                "choices": [{
                    "delta": {},
                    "index": 0,
                    "finish_reason": "stop"
                }]
            }
            yield f"data: {json.dumps(chunk)}\n\n"
            yield "data: [DONE]\n\n"

        return StreamingResponse(stream_generator(), media_type="text/event-stream")

    @staticmethod
    def _create_streaming_tool_response(
        tool_calls: List,
        event: Union[TextConversationEvent, VisionConversationEvent],
        chat_completion_id: str
    ) -> StreamingResponse:
        """Create streaming response for tool calls."""
        async def stream_generator():
            # Send role first
            chunk = {
                "id": chat_completion_id,
                "object": "chat.completion.chunk",
                "created": int(time.time()),
                "model": event.model_id,
                "choices": [{
                    "delta": {"role": "assistant"},
                    "index": 0,
                    "finish_reason": None
                }]
            }
            yield f"data: {json.dumps(chunk)}\n\n"

            # Send tool calls
            chunk = {
                "id": chat_completion_id,
                "object": "chat.completion.chunk",
                "created": int(time.time()),
                "model": event.model_id,
                "choices": [{
                    "delta": {
                        "tool_calls": [
                            {
                                "index": idx,
                                "id": tc.id,
                                "type": tc.type,
                                "function": {
                                    "name": tc.function.name,
                                    "arguments": tc.function.arguments
                                }
                            } for idx, tc in enumerate(tool_calls)
                        ]
                    },
                    "index": 0,
                    "finish_reason": "tool_calls"
                }]
            }
            yield f"data: {json.dumps(chunk)}\n\n"
            yield "data: [DONE]\n\n"

        return StreamingResponse(stream_generator(), media_type="text/event-stream")
