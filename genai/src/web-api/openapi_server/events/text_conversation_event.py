# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
TextConversationEvent: Concrete implementation for text-only LLM events.
Handles one complete turn with tool calling support.
"""

import time
import json
import asyncio
from typing import Optional, Dict, Any
from queue import Queue
from fastapi.responses import StreamingResponse

from openapi_server.events.conversation_event import (
    ConversationEvent,
    EventType,
    EventState
)
from openapi_server.impl.genie_wrapper.gen_ai_service_singleton import LLMService
from openapi_server.utils.common_utils import CommonUtils
from openapi_server.session.token_counter import TokenCounter
from openapi_server.session.tool_handler import ToolHandler
from openapi_server.managers.model_config_manager import ModelConfigManager
from openapi_server.impl.constant import LLMServiceKeys, LLMServiceQueryConstant as QUERY_CONST, SAMPLER_CONFIG_PATH
from openapi_server.logger.logger_config import LoggerConfig

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)


class TextConversationEvent(ConversationEvent):
    """
    Text conversation event representing ONE complete turn.
    Handles LLM inference, tool calling, and handle management.
    References messages in session's shared history via message_indices.
    """

    def __init__(self, session: 'ConversationSession', model_id: str):
        import uuid
        event_id = f"event-{uuid.uuid4().hex[:16]}"

        super().__init__(event_id, model_id, EventType.TEXT, session)

        # Get model context size
        config_manager = ModelConfigManager()
        self.context_size = config_manager.get_context_size(model_id)

        # Flag to track if we need to inject summary
        self.inject_summary = False

        logger.info(f"Created TextConversationEvent {event_id} for model {model_id} (context: {self.context_size})")

    def _should_summarize(self, projected_tokens: int) -> bool:
        """
        Determine if summarization should be triggered for this event.

        Args:
            projected_tokens: Projected tokens for the current request

        Returns:
            Boolean indicating if summarization should be triggered
        """
        # Skip for tool continuations
        if self._is_tool_calling and self._tool_response_received:
            return False

        # Get context size and threshold from model config
        config_manager = ModelConfigManager()
        summary_threshold_ratio = config_manager.get_summarization_threshold(self.model_id)
        threshold = self.context_size * summary_threshold_ratio

        # Calculate tokens since last summarization
        tokens_since_last_summary = self.session.calculate_tokens_since_last_summarization()
        projected_total = tokens_since_last_summary + projected_tokens

        should_summarize = projected_total >= threshold

        if should_summarize:
            logger.info(f"Event {self.event_id}: Summarization threshold reached: "
                       f"tokens_since_last={tokens_since_last_summary}, "
                       f"projected_new={projected_tokens}, "
                       f"total_projected={projected_total}, "
                       f"threshold={threshold}")

        return should_summarize

    def _calculate_projected_tokens(self, request_data) -> int:
        """
        Calculate projected tokens for the current request.

        Args:
            request_data: Request data containing messages

        Returns:
            Estimated token count for the new message
        """
        # Get last message (user message)
        last_msg = request_data.messages[-1]
        content = last_msg.content

        # Estimate tokens
        return TokenCounter.estimate_tokens_for_multimodal_content(content)

    def _find_system_prompt(self) -> Optional[Dict[str, Any]]:
        """
        Find the most recent system prompt from the conversation history.
        It scans backwards from the latest event to find a 'system' role message.

        Returns:
            The system message dict or None if not found.
        """
        if not self.session.events:
            return None

        # Iterate backwards through events
        for event in reversed(self.session.events):
            event_msgs = self.session.get_event_messages(event)
            # Check messages in this event (usually system prompt is at start of event)
            for msg in event_msgs:
                if msg.get('role') == 'system':
                    return msg
        return None

    def _build_context_for_adhoc_mode(self, current_turn_messages: list) -> list:
        """
        Build conversation context for ADHOC_MODE by including:
        1. The most recent system prompt (priority)
        2. The session summary (priority)
        3. Recent complete turns that fit within the remaining context budget.

        Strategy:
        - Identify priority components (System, Summary, Current Turn)
        - Calculate remaining budget for history
        - Work backwards through completed events
        - Include complete user-assistant pairs only
        - Skip tool calling messages
        - Stop when budget is exhausted

        Args:
            current_turn_messages: Messages for the current turn

        Returns:
            List of messages including historical context + current turn
        """
        from openapi_server.impl.constant import ADHOC_MODE

        if not ADHOC_MODE:
            return current_turn_messages

        # Skip context building during tool calling continuation
        if self._is_tool_calling and self._tool_response_received:
            logger.info(f"Event {self.event_id}: Skipping context building (tool continuation)")
            return current_turn_messages

        # Calculate context budget: 60% for input, 40% for output
        max_input_tokens = int(self.context_size * 0.6)

        # 1. Identify Priority Components
        priority_messages = []

        # System Prompt
        system_message = self._find_system_prompt()
        if system_message:
            priority_messages.append(system_message)
            logger.debug(f"Event {self.event_id}: Found system prompt for context")

        # Summary (injected as system message)
        if self.session.summary_content:
            summary_msg = {
                "role": "system",
                "content": f"Here is a summary of the conversation so far:\n{self.session.summary_content}"
            }
            priority_messages.append(summary_msg)
            logger.debug(f"Event {self.event_id}: Included summary in context")

        # Current Turn
        priority_messages.extend(current_turn_messages)

        # Estimate tokens for priority components
        priority_tokens = sum(
            TokenCounter.estimate_tokens_for_multimodal_content(msg.get('content', ''))
            for msg in priority_messages
        )

        # Calculate remaining budget for history
        history_budget = max_input_tokens - priority_tokens

        if history_budget <= 0:
            logger.warning(f"Event {self.event_id}: Priority components use all context budget ({priority_tokens} > {max_input_tokens}). Dropping history.")
            return priority_messages

        logger.info(f"Event {self.event_id}: ADHOC_MODE context building - "
                   f"max_input: {max_input_tokens}, "
                   f"priority_tokens: {priority_tokens}, "
                   f"history_budget: {history_budget} tokens")

        # Build context from completed events (work backwards)
        history_messages = []
        accumulated_tokens = 0
        events_included = 0

        for event in reversed(self.session.events):
            # Get messages for this event
            event_messages = self.session.get_event_messages(event)

            # Filter messages - keep user/assistant, EXCLUDE system (already handled)
            filtered_messages = []
            for msg in event_messages:
                role = msg.get('role', '')
                if role in ['user', 'assistant']:
                    # Skip assistant messages that only have tool_calls (no content)
                    if role == 'assistant':
                        has_content = msg.get('content') is not None and msg.get('content') != ''
                        has_only_tool_calls = msg.get('tool_calls') and not has_content
                        if has_only_tool_calls:
                            continue  # Skip tool call request messages
                    filtered_messages.append(msg)

            # Skip events that don't have relevant messages
            if not filtered_messages:
                continue

            # Estimate tokens for this event
            # Use actual token count if available, otherwise estimate
            if hasattr(event, 'total_turn_tokens') and event.total_turn_tokens > 0:
                # This count might include system tokens which we excluded, but it's a safe over-estimate
                event_tokens = event.total_turn_tokens
            else:
                event_tokens = sum(
                    TokenCounter.estimate_tokens_for_multimodal_content(msg.get('content', ''))
                    for msg in filtered_messages
                )

            # Check if adding this event would exceed budget
            if accumulated_tokens + event_tokens > history_budget:
                logger.info(f"Event {self.event_id}: Stopping context building - "
                           f"would exceed budget ({accumulated_tokens + event_tokens} > {history_budget})")
                break

            # Add event messages to history (prepend since we're going backwards)
            history_messages = filtered_messages + history_messages
            accumulated_tokens += event_tokens
            events_included += 1

            logger.debug(f"Event {self.event_id}: Added event {event.event_id} to history - "
                        f"{len(filtered_messages)} messages, {event_tokens} tokens")

        # Combine: [System] + [Summary] + [History] + [Current Turn]
        # We need to construct this carefully based on priority_messages structure
        final_context = []
        if system_message:
            final_context.append(system_message)
        if self.session.summary_content:
            final_context.append({
                "role": "system",
                "content": f"Here is a summary of the conversation so far:\n{self.session.summary_content}"
            })

        final_context.extend(history_messages)
        final_context.extend(current_turn_messages)

        logger.info(f"Event {self.event_id}: ADHOC_MODE context built - "
                   f"{len(history_messages)} historical messages from {events_included} events, "
                   f"{accumulated_tokens} history tokens, "
                   f"{len(final_context)} total messages")

        return final_context

    async def execute_turn(self, request_data) -> dict:
        """
        Execute this turn (async) with summarization support.
        May return tool_calls (turn not complete yet) or final response.
        """
        try:
            if not self.llm_handle:
                raise RuntimeError(f"Event {self.event_id}: No handle initialized")

            # Check if summarization is needed (skip for tool continuations)
            if not self._is_tool_calling and not self._tool_response_received:
                projected_tokens = self._calculate_projected_tokens(request_data)

                if self._should_summarize(projected_tokens):
                    # Step 1: Calculate max summary size (10% of context window)
                    max_summary_tokens = int(self.context_size * 0.1)
                    logger.info(f"Event {self.event_id}: Triggering summarization (max {max_summary_tokens} tokens)")

                    # Step 2: Generate summary using existing handle
                    try:
                        summary_text, summary_tokens = self.generate_summary(max_summary_tokens, include_history_in_prompt=False)

                        # Step 3: Store summary in session
                        self.session.summary_content = summary_text
                        self.session.summary_token_count = summary_tokens

                        # Step 4: Update token tracking
                        self.session.total_cumulative_tokens += summary_tokens
                        self.summarization_performed = True
                        self.summary_tokens = summary_tokens

                        # Step 5: Reset handle AFTER summarization
                        logger.info(f"Event {self.event_id}: Resetting handle after summarization")
                        self._reset_handle()
                        time.sleep(1)  # Wait for reset to complete

                        # Step 6: Set flag to inject summary in prompt
                        self.inject_summary = True

                        logger.info(f"Event {self.event_id}: Summarization complete, {summary_tokens} tokens")
                    except Exception as e:
                        logger.error(f"Summarization failed: {e}")
                        # Continue without summarization if it fails

            # Check for streaming request
            is_streaming = getattr(request_data, 'stream', False)

            if is_streaming and not self._is_tool_calling and not self._tool_response_received:
                # Execute streaming inference (only for regular turns, not tool calls for now)
                return await self._execute_streaming_inference(request_data)

            # Execute inference (with or without summary injection)
            response_content = self._execute_inference(request_data)

            if not response_content or response_content.strip() == "":
                raise ValueError("LLM returned empty response")

            # Check for tool calls
            tool_calls = ToolHandler.parse_tool_response(response_content)

            if tool_calls:
                # Tool calling - turn not complete yet
                self._is_tool_calling = True
                self._pending_tool_calls = tool_calls

                logger.info(f"Event {self.event_id}: Tool calling initiated, waiting for response")

                return {
                    "response": tool_calls,
                    "finish_reason": "tool_calls",
                    "needs_tool_response": True,
                    "turn_complete": False,
                }
            else:
                # Regular response - turn complete
                self.assistant_message = response_content
                # Note: complete_turn() will be called by the handler after adding assistant message

                logger.info(f"Event {self.event_id}: Turn completed successfully")

                return {
                    "response": response_content,
                    "finish_reason": "stop",
                    "needs_tool_response": False,
                    "turn_complete": True,
                }
        except Exception as e:
            return self._handle_error(e, request_data)

    async def _execute_streaming_inference(self, request_data) -> dict:
        """
        Execute streaming inference using thread-safe queue.
        Returns a dictionary with a StreamingResponse.
        """
        from openapi_server.impl.constant import ADHOC_MODE
        import threading
        import queue

        llm_service = LLMService()
        query = llm_service.ffi.new(LLMServiceKeys.QUERY)

        # --- Prompt Preparation (Same as _execute_inference) ---
        # Populate query fields
        CommonUtils.copy_py_string_to_c_array(
            llm_service.ffi, query.model, self.model_id, QUERY_CONST.MODEL_STR_MAX_SIZE
        )

        previous_context = ""
        # Check if we need to inject previous assistant response (if previous event involved tool calling)
        # This reinforces context even if KV cache is present (not applicable in ADHOC_MODE)
        if self.handle_borrowed and self.session.events:
            try:
                last_event = self.session.events[-1]
                # Only if it's the same model
                if last_event.model_id == self.model_id:
                    last_event_msgs = self.session.get_event_messages(last_event)
                    # Check if last event had tool calls
                    has_tool_calls = any(m.get('tool_calls') for m in last_event_msgs if m.get('role') == 'assistant')

                    if has_tool_calls:
                        # Get the final assistant response (should be the last message)
                        final_assistant_msg = None
                        for m in reversed(last_event_msgs):
                            if m.get('role') == 'assistant' and not m.get('tool_calls'):
                                final_assistant_msg = m
                                break

                        if final_assistant_msg and final_assistant_msg.get('content'):
                            previous_context = final_assistant_msg.get('content')
                            logger.info(f"Event {self.event_id}: Found previous context to inject (from tool calling): {previous_context[:50]}...")
            except Exception as e:
                logger.warning(f"Failed to check/inject previous tool response in streaming: {e}")

        # Get messages for this turn
        messages_to_format = []
        if self.message_indices:
            import copy
            for idx in self.message_indices:
                messages_to_format.append(copy.deepcopy(self.session.messages[idx]))
        else:
            msg_obj = request_data.messages[-1]
            messages_to_format.append({
                "role": getattr(msg_obj, "role", "user"),
                "content": getattr(msg_obj, "content", "")
            })

        # Build context for ADHOC_MODE (includes historical messages)
        if ADHOC_MODE:
            messages_to_format = self._build_context_for_adhoc_mode(messages_to_format)
            logger.info(f"Event {self.event_id}: ADHOC_MODE - using {len(messages_to_format)} messages in streaming prompt")
        elif self.inject_summary:
            # Non-ADHOC mode but summarization occurred (reset state).
            # Need to re-inject [System] + [Summary] + [Current Turn]
            logger.info(f"Event {self.event_id}: Rebuilding context after summarization (streaming)")

            rebuilt_messages = []

            # 1. System Prompt
            system_msg = self._find_system_prompt()
            if system_msg:
                rebuilt_messages.append(system_msg)

            # 2. Summary
            if hasattr(self.session, 'summary_content') and self.session.summary_content:
                summary_msg = {
                    "role": "system",
                    "content": f"Here is a summary of the conversation so far:\n{self.session.summary_content}"
                }
                rebuilt_messages.append(summary_msg)

            # 3. Current Turn
            rebuilt_messages.extend(messages_to_format)

            messages_to_format = rebuilt_messages
            self.inject_summary = False

        # Inject tools
        if hasattr(request_data, 'tools') and request_data.tools:
            messages_to_format = ToolHandler.inject_tool_instructions(
                messages_to_format, request_data.tools
            )

        # Prepend previous context to the first user message (not applicable in ADHOC_MODE)
        if previous_context:
            for msg in messages_to_format:
                if isinstance(msg, dict) and msg.get('role') == 'user':
                    original_content = msg.get('content', '')
                    msg['content'] = f"{previous_context} {original_content}"
                    logger.info(f"Event {self.event_id}: Prepended previous context to user message")
                    break

        # Format messages using unified builder
        formatted_content = CommonUtils.build_chat_prompt(
            model_id=self.model_id,
            messages=messages_to_format,
            include_assistant_prefix=True,
            has_vision=False
        )
        logger.info(f"Event {self.event_id}: Formatted content for Streaming LLM:\n{formatted_content}")

        CommonUtils.copy_py_string_to_c_array(llm_service.ffi, query.message.role, "user", QUERY_CONST.ROLE_MAX_SIZE)
        CommonUtils.copy_py_string_to_c_array(llm_service.ffi, query.message.content, formatted_content, QUERY_CONST.MESSAGE_CONTENT_MAX_SIZE)

        # Set parameters
        max_tokens = request_data.max_completion_tokens if request_data.max_completion_tokens else QUERY_CONST.DEFAULT_MAX_COMPLETION_TOKENS
        CommonUtils.copy_py_int_to_c_field(llm_service.ffi, query, 'max_completion_tokens', max_tokens)
        CommonUtils.copy_py_float_to_c_field(llm_service.ffi, query, 'temperature', request_data.temperature or QUERY_CONST.DEFAULT_TEMPERATURE)
        CommonUtils.copy_py_float_to_c_field(llm_service.ffi, query, 'top_p', request_data.top_p or QUERY_CONST.DEFAULT_TOP_P)
        CommonUtils.copy_py_float_to_c_field(llm_service.ffi, query, 'frequency_penalty', request_data.frequency_penalty or QUERY_CONST.DEFAULT_FREQUENCY_PENALTY)
        CommonUtils.copy_py_float_to_c_field(llm_service.ffi, query, 'presence_penalty', request_data.presence_penalty or QUERY_CONST.DEFAULT_PRESENCE_PENALTY)
        # -------------------------------------------------------

        # Thread-safe queue for tokens
        token_queue = queue.Queue()

        @llm_service.ffi.callback("void(const Response *)")
        def callback(response_ptr):
            try:
                resp = response_ptr[0]
                choice = resp.choices[0]
                msg = choice.message
                content = llm_service.ffi.string(msg.content).decode("utf-8")
                finish_reason = llm_service.ffi.string(choice.finish_reason).decode("utf-8")

                # Put token in queue (thread-safe)
                if content:
                    token_queue.put(content)

                # Check for completion signal from C++ callback
                if finish_reason == "stop" and not content:
                    token_queue.put(None)  # Sentinel for completion
            except Exception as e:
                logger.error(f"Error in C callback: {e}")
                token_queue.put(f"__ERROR__{str(e)}")
                token_queue.put(None)

        def run_llm():
            try:
                logger.info(f"Event {self.event_id}: Starting streaming inference thread")
                # This blocks until streaming completes in C++
                llm_service.lib.llm_chat_completion_create(
                    self.llm_handle, query, True, callback
                )
                logger.info(f"Event {self.event_id}: Streaming inference thread finished")
            except Exception as e:
                logger.error(f"Error in streaming C call: {e}")
                token_queue.put(f"__ERROR__{str(e)}")
                token_queue.put(None)

        # Start LLM in background thread
        worker_thread = threading.Thread(target=run_llm, daemon=True)
        start_time = time.time()
        worker_thread.start()

        # Create async generator to consume queue
        async def stream_generator():
            accumulated_content = ""
            completion_tokens = 0
            created_time = int(time.time())

            # Metrics tracking
            first_token_time = None

            # Buffering logic for tool detection
            # We buffer initial tokens to check if it's a JSON response (potential tool call)
            # If it is, we buffer the whole thing to parse it.
            # If not, we flush the buffer and stream normally.
            response_buffer = []
            is_checking_tool = True
            is_buffering_tool = False

            # Yield role chunk immediately
            first_chunk = {
                "id": self.session.session_id,
                "object": "chat.completion.chunk",
                "created": created_time,
                "model": self.model_id,
                "choices": [{
                    "index": 0,
                    "delta": {"role": "assistant", "content": None},
                    "finish_reason": None,
                    "logprobs": None
                }]
            }
            yield f"data: {json.dumps(first_chunk)}\n\n"

            try:
                while True:
                    # Poll queue with timeout (non-blocking for asyncio loop)
                    try:
                        token = token_queue.get(timeout=0.1)
                    except queue.Empty:
                        if not worker_thread.is_alive() and token_queue.empty():
                            # Thread died without sending None?
                            logger.warning(f"Event {self.event_id}: Worker thread died unexpectedly")
                            break
                        await asyncio.sleep(0.01) # Yield to event loop
                        continue

                    if token is None:
                        break

                    if isinstance(token, str) and token.startswith("__ERROR__"):
                        logger.error(f"Error in backend streaming: {token}")
                        error_msg = token.replace("__ERROR__", "")
                        yield f"data: {json.dumps({'error': error_msg})}\n\n"
                        break

                    if first_token_time is None:
                        first_token_time = time.time()
                        ttft = first_token_time - start_time
                        logger.info(f"Event {self.event_id}: Time to First Token (TTFT): {ttft:.4f}s")

                    accumulated_content += token
                    completion_tokens += 1

                    if is_checking_tool:
                        response_buffer.append(token)
                        current_buffer_str = "".join(response_buffer)

                        # Wait for first non-whitespace char
                        if not current_buffer_str.strip():
                            continue

                        if current_buffer_str.strip().startswith('{'):
                            # Looks like JSON start, enable full buffering
                            is_checking_tool = False
                            is_buffering_tool = True
                            continue
                        else:
                            # Not JSON, flush buffer and disable checking
                            is_checking_tool = False
                            is_buffering_tool = False

                            # Flush buffer
                            for buf_token in response_buffer:
                                content_chunk = {
                                    "id": self.session.session_id,
                                    "object": "chat.completion.chunk",
                                    "created": created_time,
                                    "model": self.model_id,
                                    "choices": [{
                                        "index": 0,
                                        "delta": {"content": buf_token},
                                        "finish_reason": None,
                                        "logprobs": None
                                    }]
                                }
                                yield f"data: {json.dumps(content_chunk)}\n\n"
                            continue

                    if is_buffering_tool:
                        # We are buffering to check for tool calls at the end
                        continue

                    # Normal streaming
                    content_chunk = {
                        "id": self.session.session_id,
                        "object": "chat.completion.chunk",
                        "created": created_time,
                        "model": self.model_id,
                        "choices": [{
                            "index": 0,
                            "delta": {"content": token},
                            "finish_reason": None,
                            "logprobs": None
                        }]
                    }
                    yield f"data: {json.dumps(content_chunk)}\n\n"

            except Exception as e:
                logger.error(f"Error in stream generator: {e}")
                yield f"data: {json.dumps({'error': str(e)})}\n\n"

            finally:
                # Cleanup
                worker_thread.join(timeout=2.0)
                if worker_thread.is_alive():
                    logger.warning(f"Backend thread for {self.event_id} did not terminate within timeout")

                # Check for tool calls if we were buffering
                tool_calls = None
                if is_buffering_tool or is_checking_tool: # is_checking_tool could be true if response was empty or whitespace
                    # Attempt to parse
                    tool_calls = ToolHandler.parse_tool_response(accumulated_content)

                if tool_calls:
                    # It IS a tool call!
                    self._is_tool_calling = True
                    self._pending_tool_calls = tool_calls
                    self.assistant_message = None # No text content for tool call turn (or maybe we keep it?)

                    logger.info(f"Event {self.event_id}: Detected tool calls in streaming response")

                    # Convert tool calls for response
                    tool_calls_data = []
                    for i, tc in enumerate(tool_calls):
                        tc_dict = {
                            "index": i,
                            "id": tc.id,
                            "type": tc.type,
                            "function": {
                                "name": tc.function.name,
                                "arguments": tc.function.arguments
                            }
                        }
                        tool_calls_data.append(tc_dict)

                    # Send tool calls chunk
                    tc_chunk = {
                        "id": self.session.session_id,
                        "object": "chat.completion.chunk",
                        "created": created_time,
                        "model": self.model_id,
                        "choices": [{
                            "index": 0,
                            "delta": {"tool_calls": tool_calls_data},
                            "finish_reason": None,
                            "logprobs": None
                        }]
                    }
                    yield f"data: {json.dumps(tc_chunk)}\n\n"

                    # Add assistant message with tool calls to session
                    # We need to do this manually here since we bypassed the normal turn completion logic for tool calls
                    assistant_msg = {
                        "role": "assistant",
                        "content": None,
                        "tool_calls": tool_calls # Stores Pydantic objects, which is consistent with existing code
                    }
                    assistant_idx = self.session.add_message(assistant_msg)
                    self.message_indices.append(assistant_idx)

                    # Register for tool calling (needed for session management)
                    # Use ONLY user messages for stable hash during tool calling
                    from openapi_server.session.conversation_utils import ConversationUtils
                    from openapi_server.managers.session_manager import SessionManager

                    user_message_indices = [idx for idx in self.message_indices
                                           if self.session.messages[idx].get('role') == 'user']
                    user_messages = [self.session.messages[idx] for idx in user_message_indices]
                    event_hash = ConversationUtils.calculate_hash_for_specific_messages(user_messages)

                    session_manager = SessionManager.get_instance()
                    session_manager.register_tool_calling_event(event_hash, self.session.session_id)

                    finish_reason = "tool_calls"

                else:
                    # Not a tool call (or failed to parse)
                    finish_reason = "stop"

                    # If we were buffering but failed to parse as tool, we need to flush the accumulated content
                    # Note: accumulated_content contains EVERYTHING
                    if is_buffering_tool:
                        # We buffered everything, so now we stream it all out as text
                        # chunk by chunk to simulate streaming (fast)
                        # or just one big chunk
                        chunk = {
                            "id": self.session.session_id,
                            "object": "chat.completion.chunk",
                            "created": created_time,
                            "model": self.model_id,
                            "choices": [{
                                "index": 0,
                                "delta": {"content": accumulated_content},
                                "finish_reason": None,
                                "logprobs": None
                            }]
                        }
                        yield f"data: {json.dumps(chunk)}\n\n"

                    # Store accumulated content
                    self.assistant_message = accumulated_content

                    # Add assistant message to session
                    assistant_msg = {
                        "role": "assistant",
                        "content": accumulated_content
                    }
                    assistant_idx = self.session.add_message(assistant_msg)
                    self.message_indices.append(assistant_idx)

                    # Complete event
                    self.complete_turn()
                    self.calculate_event_hash()
                    self.session.complete_current_event()

                # Final chunk
                final_chunk = {
                    "id": self.session.session_id,
                    "object": "chat.completion.chunk",
                    "created": created_time,
                    "model": self.model_id,
                    "choices": [{
                        "index": 0,
                        "delta": {},
                        "finish_reason": finish_reason,
                        "logprobs": None
                    }]
                }
                yield f"data: {json.dumps(final_chunk)}\n\n"
                yield "data: [DONE]\n\n"

                end_time = time.time()
                generation_time = end_time - (first_token_time if first_token_time else start_time)
                tps = completion_tokens / generation_time if generation_time > 0 else 0
                logger.info(f"Event {self.event_id}: Streaming completed, {completion_tokens} tokens (finish_reason: {finish_reason})")
                logger.info(f"Event {self.event_id}: Tokens Per Second (TPS): {tps:.2f} tokens/s")

                # CRITICAL: Trigger completion callback to release DSP lock
                # This must happen after streaming completes to ensure proper lock management
                if self._completion_callback:
                    logger.info(f"Event {self.event_id}: Triggering completion callback from streaming generator")
                    try:
                        await self._completion_callback(self.event_id, self.state)
                    except Exception as e:
                        logger.error(f"Event {self.event_id}: Error in streaming completion callback: {e}", exc_info=True)

        # Set SSE/blocking headers
        response_headers = {
            "Cache-Control": "no-cache",
            "X-Accel-Buffering": "no"
        }

        return {
            "response": StreamingResponse(
                stream_generator(),
                media_type="text/event-stream",
                headers=response_headers
            ),
            "finish_reason": "stop",
            "needs_tool_response": False,
            "turn_complete": True,
            "is_streaming": True
        }

    async def continue_with_tool_response(self, tool_response: str, request_data) -> dict:
        """
        Continue turn after receiving tool response (async).
        Completes the turn.
        """
        try:
            if not self._is_tool_calling:
                raise RuntimeError(f"Event {self.event_id}: Not in tool calling state")

            # Store tool response
            self.tool_info = tool_response
            self._tool_response_received = True

            logger.info(f"Event {self.event_id}: Received tool response, generating final answer")

            # Execute final inference with tool result
            final_response = self._execute_inference_with_tool(
                self.user_message,
                tool_response,
                request_data
            )

            if not final_response or final_response.strip() == "":
                raise ValueError("LLM returned empty response after tool call")

            # Turn complete
            self.assistant_message = final_response
            self._is_tool_calling = False

            # Note: complete_turn() will be called by the handler after adding final assistant message

            logger.info(f"Event {self.event_id}: Turn completed after tool calling")

            return {
                "response": final_response,
                "finish_reason": "stop",
                "needs_tool_response": False,
                "turn_complete": True
            }

        except Exception as e:
            return self._handle_error(e, request_data)

    def _execute_inference(self, request_data) -> str:
        """Execute inference for this turn."""
        from openapi_server.impl.constant import ADHOC_MODE

        llm_service = LLMService()
        query = llm_service.ffi.new(LLMServiceKeys.QUERY)

        # Populate query fields
        CommonUtils.copy_py_string_to_c_array(
            llm_service.ffi, query.model, self.model_id, QUERY_CONST.MODEL_STR_MAX_SIZE
        )

        previous_context = ""
        # Check if we need to inject previous assistant response (if previous event involved tool calling)
        # This reinforces context even if KV cache is present (not applicable in ADHOC_MODE)
        if self.handle_borrowed and self.session.events:
            try:
                last_event = self.session.events[-1]
                # Only if it's the same model (implicit in handle_borrowed, but good to check)
                if last_event.model_id == self.model_id:
                    last_event_msgs = self.session.get_event_messages(last_event)
                    # Check if last event had tool calls
                    has_tool_calls = any(m.get('tool_calls') for m in last_event_msgs if m.get('role') == 'assistant')

                    if has_tool_calls:
                        # Get the final assistant response (should be the last message)
                        # We look for the last assistant message that is NOT a tool call request
                        final_assistant_msg = None
                        for m in reversed(last_event_msgs):
                            if m.get('role') == 'assistant' and not m.get('tool_calls'):
                                final_assistant_msg = m
                                break

                        if final_assistant_msg and final_assistant_msg.get('content'):
                            previous_context = final_assistant_msg.get('content')
                            logger.info(f"Event {self.event_id}: Found previous context to inject (from tool calling): {previous_context[:50]}...")
            except Exception as e:
                logger.warning(f"Failed to check/inject previous tool response: {e}")

        # Get messages for this turn
        messages_to_format = []
        if self.message_indices:
            # Get messages from session using indices
            # We must use a deep copy because inject_tool_instructions modifies in-place
            import copy
            for idx in self.message_indices:
                messages_to_format.append(copy.deepcopy(self.session.messages[idx]))
        else:
            # Fallback to request data (shouldn't happen normally)
            msg_obj = request_data.messages[-1]
            messages_to_format.append({
                "role": getattr(msg_obj, "role", "user"),
                "content": getattr(msg_obj, "content", "")
            })

        # Build context for ADHOC_MODE (includes historical messages)
        if ADHOC_MODE:
            messages_to_format = self._build_context_for_adhoc_mode(messages_to_format)
            logger.info(f"Event {self.event_id}: ADHOC_MODE - using {len(messages_to_format)} messages in prompt")
        elif self.inject_summary:
            # Non-ADHOC mode but summarization occurred (reset state).
            # Need to re-inject [System] + [Summary] + [Current Turn]
            logger.info(f"Event {self.event_id}: Rebuilding context after summarization")

            rebuilt_messages = []

            # 1. System Prompt
            system_msg = self._find_system_prompt()
            if system_msg:
                rebuilt_messages.append(system_msg)

            # 2. Summary
            if hasattr(self.session, 'summary_content') and self.session.summary_content:
                summary_msg = {
                    "role": "system",
                    "content": f"Here is a summary of the conversation so far:\n{self.session.summary_content}"
                }
                rebuilt_messages.append(summary_msg)

            # 3. Current Turn
            rebuilt_messages.extend(messages_to_format)

            messages_to_format = rebuilt_messages
            self.inject_summary = False

        # Always inject tools via a system message for consistency.
        if hasattr(request_data, 'tools') and request_data.tools:
            tools_count = len(request_data.tools)
            logger.info(f"Event {self.event_id}: Injecting {tools_count} tools via System Prompt.")
            messages_to_format = ToolHandler.inject_tool_instructions(
                messages_to_format, request_data.tools
            )

        # Prepend previous context to the first user message (not applicable in ADHOC_MODE)
        if previous_context:
            for msg in messages_to_format:
                if isinstance(msg, dict) and msg.get('role') == 'user':
                    original_content = msg.get('content', '')
                    msg['content'] = f"{previous_context} {original_content}"
                    logger.info(f"Event {self.event_id}: Prepended previous context to user message")
                    break

        # Format all messages for this turn using unified builder
        formatted_content = CommonUtils.build_chat_prompt(
            model_id=self.model_id,
            messages=messages_to_format,
            include_assistant_prefix=True,
            has_vision=False
        )

        logger.info(f"Event {self.event_id}: Formatted content for LLM:\n{formatted_content}")

        # This seems wrong, the query struct has role and content, but we are combining them.
        # The old chat_utils code did this:
        # CommonUtils.copy_py_string_to_c_array(llm_service.ffi, query.message.role, last_msg.role, QUERY_CONST.ROLE_MAX_SIZE)
        # CommonUtils.copy_py_string_to_c_array(llm_service.ffi, query.message.content, formatted_content, QUERY_CONST.MESSAGE_CONTENT_MAX_SIZE)
        # So it seems the `role` is just the last message's role, and `content` is the full templated history. Let's do that.
        # With the new logic, `last_msg` is not available. Using "user" as a placeholder, as the templated content contains the actual roles.
        CommonUtils.copy_py_string_to_c_array(llm_service.ffi, query.message.role, "user", QUERY_CONST.ROLE_MAX_SIZE)
        CommonUtils.copy_py_string_to_c_array(llm_service.ffi, query.message.content, formatted_content, QUERY_CONST.MESSAGE_CONTENT_MAX_SIZE)

        # Set parameters
        max_tokens = request_data.max_completion_tokens if request_data.max_completion_tokens else QUERY_CONST.DEFAULT_MAX_COMPLETION_TOKENS
        CommonUtils.copy_py_int_to_c_field(llm_service.ffi, query, 'max_completion_tokens', max_tokens)
        CommonUtils.copy_py_float_to_c_field(
            llm_service.ffi, query, 'temperature',
            request_data.temperature or QUERY_CONST.DEFAULT_TEMPERATURE
        )
        CommonUtils.copy_py_float_to_c_field(
            llm_service.ffi, query, 'top_p',
            request_data.top_p or QUERY_CONST.DEFAULT_TOP_P
        )
        CommonUtils.copy_py_float_to_c_field(
            llm_service.ffi, query, 'frequency_penalty',
            request_data.frequency_penalty or QUERY_CONST.DEFAULT_FREQUENCY_PENALTY
        )
        CommonUtils.copy_py_float_to_c_field(
            llm_service.ffi, query, 'presence_penalty',
            request_data.presence_penalty or QUERY_CONST.DEFAULT_PRESENCE_PENALTY
        )

        # Execute inference (accumulate tokens)
        accumulated_tokens = []
        import threading
        completion_event = threading.Event()

        # Metrics tracking
        timing_stats = {
            "start_time": time.time(),
            "first_token_time": None,
            "token_count": 0
        }

        @llm_service.ffi.callback("void(const Response *)")
        def callback(response_ptr):
            try:
                if timing_stats["first_token_time"] is None:
                    timing_stats["first_token_time"] = time.time()
                    ttft = timing_stats["first_token_time"] - timing_stats["start_time"]
                    logger.info(f"Event {self.event_id}: Time to First Token (TTFT): {ttft:.4f}s")

                resp = response_ptr[0]
                choice = resp.choices[0]
                msg = choice.message
                content = llm_service.ffi.string(msg.content).decode("utf-8")
                finish_reason = llm_service.ffi.string(choice.finish_reason).decode("utf-8")

                if content:
                    accumulated_tokens.append(content)
                    timing_stats["token_count"] += 1

                if finish_reason == "stop":
                    completion_event.set()
            except Exception as e:
                logger.error(f"Callback error: {e}")
                completion_event.set()

        llm_service.lib.llm_chat_completion_create(
            self.llm_handle, query, False, callback
        )

        # Wait for completion (with timeout)
        if not completion_event.wait(timeout=60):
            logger.error(f"Event {self.event_id}: Non-streaming inference timed out")
            # Return what we have so far

        response_content = "".join(accumulated_tokens)

        end_time = time.time()
        generation_time = end_time - (timing_stats["first_token_time"] if timing_stats["first_token_time"] else timing_stats["start_time"])
        tps = timing_stats["token_count"] / generation_time if generation_time > 0 else 0

        logger.debug(f"Event {self.event_id}: Inference completed, {len(response_content)} chars")
        logger.info(f"Event {self.event_id}: Tokens Per Second (TPS): {tps:.2f} tokens/s")

        return response_content

    def _execute_inference_with_tool(self, user_msg: str, tool_result: str, request_data) -> str:
        """Execute inference with tool result."""
        llm_service = LLMService()
        query = llm_service.ffi.new(LLMServiceKeys.QUERY)

        # Populate query
        CommonUtils.copy_py_string_to_c_array(
            llm_service.ffi, query.model, self.model_id, QUERY_CONST.MODEL_STR_MAX_SIZE
        )

        # Build prompt with tool result
        formatted_parts = []

        # We need the previous messages for context
        # Let's rebuild the context from the session messages for this event
        import copy
        context_messages = copy.deepcopy(self.session.get_event_messages(self))

        # Inject tools if provided
        if hasattr(request_data, 'tools') and request_data.tools:
            has_system = any(m.get('role') == 'system' for m in context_messages)
            if has_system:
                logger.info(f"Event {self.event_id}: Injecting tool instructions into context for tool execution")
                context_messages = ToolHandler.inject_tool_instructions(
                    context_messages, request_data.tools
                )

        # Add tool response as a user message for context
        tool_response_with_instruction = (
            f"{tool_result}\n\n"
            "Based on the tool result above, please provide a helpful response to the user's question."
        )

        # Create a list of messages for the prompt builder
        # Filter out existing 'tool' messages from context as we only want the current result
        prompt_messages = [
            msg for msg in context_messages
            if (isinstance(msg, dict) and msg.get('role') != 'tool') or
               (not isinstance(msg, dict) and getattr(msg, 'role', '') != 'tool')
        ]

        # Append the tool result as a user message
        prompt_messages.append({
            "role": "user",
            "content": tool_response_with_instruction
        })

        # Use unified builder
        formatted_content = CommonUtils.build_chat_prompt(
            model_id=self.model_id,
            messages=prompt_messages,
            include_assistant_prefix=True,
            has_vision=False
        )

        logger.info(f"Event {self.event_id}: Formatted content for LLM (with tool response):\n{formatted_content}")

        CommonUtils.copy_py_string_to_c_array(
            llm_service.ffi, query.message.role, "user", QUERY_CONST.ROLE_MAX_SIZE # Role is not that important here
        )
        CommonUtils.copy_py_string_to_c_array(
            llm_service.ffi, query.message.content, formatted_content, QUERY_CONST.MESSAGE_CONTENT_MAX_SIZE
        )

        # Set parameters
        max_tokens = request_data.max_completion_tokens if request_data.max_completion_tokens else QUERY_CONST.DEFAULT_MAX_COMPLETION_TOKENS
        CommonUtils.copy_py_int_to_c_field(llm_service.ffi, query, 'max_completion_tokens', max_tokens)
        CommonUtils.copy_py_float_to_c_field(
            llm_service.ffi, query, 'temperature',
            request_data.temperature or QUERY_CONST.DEFAULT_TEMPERATURE
        )

        # Execute (accumulate tokens)
        accumulated_tokens = []
        import threading
        completion_event = threading.Event()

        # Metrics tracking
        timing_stats = {
            "start_time": time.time(),
            "first_token_time": None,
            "token_count": 0
        }

        @llm_service.ffi.callback("void(const Response *)")
        def callback(response_ptr):
            try:
                if timing_stats["first_token_time"] is None:
                    timing_stats["first_token_time"] = time.time()
                    ttft = timing_stats["first_token_time"] - timing_stats["start_time"]
                    logger.info(f"Event {self.event_id}: Time to First Token (TTFT): {ttft:.4f}s")

                resp = response_ptr[0]
                choice = resp.choices[0]
                msg = choice.message
                content = llm_service.ffi.string(msg.content).decode("utf-8")
                finish_reason = llm_service.ffi.string(choice.finish_reason).decode("utf-8")

                if content:
                    accumulated_tokens.append(content)
                    timing_stats["token_count"] += 1

                if finish_reason == "stop":
                    completion_event.set()
            except Exception as e:
                logger.error(f"Callback error in tool follow-up: {e}")
                completion_event.set()

        llm_service.lib.llm_chat_completion_create(
            self.llm_handle, query, False, callback
        )

        # Wait for completion (with timeout)
        if not completion_event.wait(timeout=60):
            logger.error(f"Event {self.event_id}: Tool follow-up inference timed out")

        response_content = "".join(accumulated_tokens)

        end_time = time.time()
        generation_time = end_time - (timing_stats["first_token_time"] if timing_stats["first_token_time"] else timing_stats["start_time"])
        tps = timing_stats["token_count"] / generation_time if generation_time > 0 else 0

        logger.debug(f"Event {self.event_id}: Tool follow-up completed, {len(response_content)} chars")
        logger.info(f"Event {self.event_id}: Tokens Per Second (TPS): {tps:.2f} tokens/s")

        return response_content

    def _handle_error(self, error: Exception, request_data) -> dict:
        """Handle error with retry logic."""
        self.retry_count += 1

        logger.error(f"Event {self.event_id}: Error (attempt {self.retry_count}/{self.max_retries}): {error}")

        is_retryable = self._is_retryable_error(error)

        if is_retryable and self.retry_count < self.max_retries:
            logger.info(f"Event {self.event_id}: Retrying...")

            try:
                # Reset handle
                self._reset_handle()

                # Retry
                if self._tool_response_received:
                    return self.continue_with_tool_response(self.tool_info, request_data)
                else:
                    return self.execute_turn(request_data)

            except Exception as retry_error:
                logger.error(f"Event {self.event_id}: Retry failed: {retry_error}")

        # Mark as failed
        self.fail_turn(error)

        return {
            "response": None,
            "finish_reason": "error",
            "needs_tool_response": False,
            "turn_complete": False,
            "error": {
                "type": type(error).__name__,
                "message": str(error)
            }
        }

    def _is_retryable_error(self, error: Exception) -> bool:
        """Check if error is retryable."""
        retryable_types = (
            TimeoutError,
            ConnectionError,
            ValueError  # Empty response
        )

        non_retryable_keywords = [
            "out of memory",
            "invalid model",
            "handle creation failed",
            "configuration error"
        ]

        error_msg = str(error).lower()

        # Check if error message contains non-retryable keywords
        if any(keyword in error_msg for keyword in non_retryable_keywords):
            return False

        # Check if error type is retryable
        return isinstance(error, retryable_types)

    def take_over_handle(self, previous_event: ConversationEvent):
        """Take over handle from previous event."""
        if previous_event.model_id != self.model_id:
            raise ValueError(
                f"Cannot take over handle: model mismatch ({previous_event.model_id} != {self.model_id})"
            )

        self.llm_handle = previous_event.llm_handle
        self.handle_borrowed = True
        self.handle_owned = False

        logger.info(f"Event {self.event_id}: Took over handle from {previous_event.event_id}")

    def create_new_handle(self):
        """Create new handle for this event with simplified error handling."""
        from openapi_server.impl.constant import ADHOC_MODE

        # Check circuit breaker
        if LLMService.is_circuit_breaker_open():
            error_msg = LLMService.get_initialization_error()
            if not error_msg:
                error_msg = "Service temporarily unavailable due to repeated model loading failures"
            logger.error(f"Event {self.event_id}: {error_msg}")
            raise RuntimeError(error_msg)

        try:
            llm_service = LLMService()
            config_path = CommonUtils.get_model_config_path(self.model_id)

            if ADHOC_MODE:
                # Use caching wrapper for ADHOC mode
                self.llm_handle = LLMService.get_or_create_handle(
                    self.model_id, config_path, streaming=True
                )
                logger.info(f"Event {self.event_id}: Got handle from ADHOC cache for model {self.model_id}")
            else:
                # Direct creation for normal mode
                model_input = llm_service.ffi.new("char[]", self.model_id.encode('utf-8'))
                config_path_input = llm_service.ffi.new("char[]", config_path.encode('utf-8'))
                sampler_path = SAMPLER_CONFIG_PATH
                sampler_input = llm_service.ffi.new("char[]", sampler_path.encode('utf-8'))

                self.llm_handle = llm_service.lib.llm_create_object(
                    model_input, config_path_input, sampler_input, True
                )

            # Simple NULL check - no function calls to undefined C functions
            if self.llm_handle == llm_service.ffi.NULL:
                error_msg = f"Failed to create LLM handle for model {self.model_id} (NULL handle returned)"
                logger.error(f"Event {self.event_id}: {error_msg}")
                LLMService.record_failure(error_msg)
                raise RuntimeError(error_msg)

            # Success - reset failure count
            LLMService.record_success()
            self.handle_owned = True
            self.handle_borrowed = False

            logger.info(f"Event {self.event_id}: Created new handle for model {self.model_id} (streaming mode)")

        except RuntimeError:
            raise
        except Exception as e:
            error_msg = f"Failed to create LLM handle: {str(e)}"
            logger.error(f"Event {self.event_id}: Unexpected error creating handle: {e}", exc_info=True)
            LLMService.record_failure(error_msg)
            raise RuntimeError(error_msg)

    def release_handle(self):
        """Release handle."""
        from openapi_server.impl.constant import ADHOC_MODE

        if ADHOC_MODE:
            # In ADHOC mode, we don't destroy handles on release (they are cached)
            logger.info(f"Event {self.event_id}: Released handle (kept in ADHOC cache)")
            self.llm_handle = None
            return

        if self.llm_handle and self.handle_owned:
            llm_service = LLMService()
            llm_service.lib.llm_destroy_object(self.llm_handle)
            logger.info(f"Event {self.event_id}: Released owned handle")
        elif self.llm_handle and self.handle_borrowed:
            logger.info(f"Event {self.event_id}: Released borrowed handle (not destroyed)")

        self.llm_handle = None

    def terminate_handle(self):
        """Forcefully destroy the handle, regardless of ownership."""
        from openapi_server.impl.constant import ADHOC_MODE

        if ADHOC_MODE:
            # In ADHOC mode, we specifically destroy the cached handle for this model
            logger.info(f"Event {self.event_id}: Terminating cached handle for model {self.model_id}")
            LLMService.destroy_cached_handle(self.model_id)
            self.llm_handle = None
            return

        if self.llm_handle:
            llm_service = LLMService()
            # We are terminating, so we destroy the handle even if borrowed
            llm_service.lib.llm_destroy_object(self.llm_handle)
            logger.info(f"Event {self.event_id}: Terminated handle (owned={self.handle_owned}, borrowed={self.handle_borrowed})")
            self.llm_handle = None
            self.handle_owned = False
            self.handle_borrowed = False

    def _reset_handle(self):
        """Reset handle to clear KV cache."""
        if self.llm_handle:
            llm_service = LLMService()
            llm_service.lib.llm_reset_object(self.llm_handle)
            logger.debug(f"Event {self.event_id}: Handle reset")

    def _calculate_prompt_tokens(self) -> int:
        """Calculate prompt tokens from input messages for this turn."""
        tokens = 0

        # Get messages this event is responsible for (user messages)
        for idx in self.message_indices:
            if idx < len(self.session.messages):
                msg = self.session.messages[idx]
                if msg.get('role') == 'user':
                    # Add role formatting tokens (4 tokens per message)
                    tokens += 4
                    # Add content tokens
                    content = msg.get('content', '')
                    tokens += TokenCounter.estimate_tokens(content)

        logger.debug(f"Event {self.event_id}: Calculated {tokens} prompt tokens")
        return tokens

    def _calculate_completion_tokens(self) -> int:
        """Calculate completion tokens from assistant response and tool overhead."""
        tokens = 0

        # Get messages this event is responsible for
        for idx in self.message_indices:
            if idx < len(self.session.messages):
                msg = self.session.messages[idx]
                role = msg.get('role', '')

                if role == 'assistant':
                    # Assistant message content tokens
                    content = msg.get('content', '')
                    if content:
                        tokens += TokenCounter.estimate_tokens(content)

                    # Tool calls tokens (if present)
                    tool_calls = msg.get('tool_calls', [])
                    if tool_calls:
                        # Convert tool calls to dicts if they're Pydantic objects
                        tool_calls_dicts = []
                        for tc in tool_calls:
                            if hasattr(tc, 'to_dict'):
                                tool_calls_dicts.append(tc.to_dict())
                            elif hasattr(tc, 'model_dump'):
                                tool_calls_dicts.append(tc.model_dump())
                            elif isinstance(tc, dict):
                                tool_calls_dicts.append(tc)
                            else:
                                try:
                                    tool_calls_dicts.append(dict(tc))
                                except:
                                    tool_calls_dicts.append(str(tc))

                        # Convert to string and estimate tokens
                        tool_calls_str = json.dumps(tool_calls_dicts)
                        tokens += TokenCounter.estimate_tokens(tool_calls_str)

                elif role == 'tool':
                    # Tool response tokens
                    content = msg.get('content', '')
                    if content:
                        tokens += TokenCounter.estimate_tokens(content)
                    # Tool message formatting overhead
                    tokens += 4

        logger.debug(f"Event {self.event_id}: Calculated {tokens} completion tokens")
        return tokens

    def generate_summary(self, max_summary_tokens: int, messages_to_summarize: Optional[list] = None, include_history_in_prompt: bool = True) -> tuple:
        """
        Generate a summary of a conversation using the existing LLM handle.

        Args:
            max_summary_tokens: Maximum tokens for the summary
            messages_to_summarize: Optional list of messages to summarize.
                                   If None, summarizes the entire session.
            include_history_in_prompt: If True, include conversation history in the prompt.
                                     If False, rely on the LLM handle's existing context.

        Returns:
            Tuple of (summary_text, summary_tokens)
        """
        if not self.llm_handle:
            raise RuntimeError("Cannot generate summary without an active LLM handle.")

        summary_prompt = ""

        if include_history_in_prompt:
            # Determine which messages to use
            source_messages = messages_to_summarize if messages_to_summarize is not None else self.session.messages

            # Get relevant messages from session for context
            relevant_messages = []
            for msg in source_messages:
                role = msg.get('role', '')

                # Skip tool response messages (they're redundant with assistant's final response)
                if role == 'tool':
                    continue

                # Include all other messages, including assistant messages with tool calls
                relevant_messages.append(msg)

            # Limit to last 2 message pairs (4 messages) to prevent context overflow
            if len(relevant_messages) > 4:
                logger.info(f"Summarization context too long ({len(relevant_messages)} msgs), limiting to last 4")
                relevant_messages = relevant_messages[-4:]

            # Create context string from relevant messages
            context_parts = []
            for msg in relevant_messages:
                role = msg.get('role', '')
                content = msg.get('content', '')
                tool_calls = msg.get('tool_calls', [])

                # Handle content
                content_text = ""
                if isinstance(content, str) and content:
                    content_text = content
                elif isinstance(content, list):
                    # Extract text from multimodal content
                    text_parts = []
                    for item in content:
                        if isinstance(item, dict) and item.get('type') == 'text':
                            text_parts.append(item.get('text', ''))
                    if text_parts:
                        content_text = ' '.join(text_parts)

                # Handle tool calls (convert to readable format for summarization)
                tool_call_text = ""
                if tool_calls:
                    tool_descriptions = []
                    for tc in tool_calls:
                        if isinstance(tc, dict):
                            func = tc.get('function', {})
                            if isinstance(func, dict):
                                func_name = func.get('name', '')
                                func_args = func.get('arguments', '')
                            else:
                                func_name = getattr(func, 'name', '')
                                func_args = getattr(func, 'arguments', '')
                        else:
                            func = getattr(tc, 'function', None)
                            func_name = getattr(func, 'name', '') if func else ''
                            func_args = getattr(func, 'arguments', '') if func else ''

                        if func_name:
                            tool_descriptions.append(f"called function '{func_name}' with arguments: {func_args}")

                    if tool_descriptions:
                        tool_call_text = " [" + "; ".join(tool_descriptions) + "]"

                # Combine content and tool calls
                if content_text or tool_call_text:
                    message_text = f"{role.capitalize()}: {content_text}{tool_call_text}"
                    context_parts.append(message_text)

            conversation_context = "\n\n".join(context_parts)

            summary_prompt = (
                "Below is a conversation that needs to be summarized:\n\n"
                f"{conversation_context}\n\n"
                "Please provide a concise summary of the conversation above. "
                f"The summary should be no more than {max_summary_tokens} tokens. "
                "Focus on the key points and important details. "
                "IMPORTANT: If the conversation includes descriptions of images or visual content, "
                "preserve these details in the summary as they provide essential context. "
                "Maintain the strict chronological order of events in your summary. "
                "Place special emphasis on the most recent interactions and topics, as they constitute "
                "the immediate context for the next response. "
                "Include all relevant information that would help continue the conversation naturally."
            )
        else:
            # Simplified prompt relying on handle context
            summary_prompt = (
                "Please provide a concise summary of the conversation above. "
                f"The summary should be no more than {max_summary_tokens} tokens. "
                "Focus on the key points and important details. "
                "IMPORTANT: If the conversation includes descriptions of images or visual content, "
                "preserve these details in the summary as they provide essential context. "
                "Maintain the strict chronological order of events in your summary. "
                "Place special emphasis on the most recent interactions and topics, as they constitute "
                "the immediate context for the next response. "
                "Include all relevant information that would help continue the conversation naturally."
            )

        llm_service = LLMService()
        query = llm_service.ffi.new(LLMServiceKeys.QUERY)

        # Populate query fields
        CommonUtils.copy_py_string_to_c_array(
            llm_service.ffi, query.model, self.model_id, QUERY_CONST.MODEL_STR_MAX_SIZE
        )

        # Format the prompt using the unified builder
        formatted_prompt = CommonUtils.build_chat_prompt(
            model_id=self.model_id,
            messages=[{"role": "user", "content": summary_prompt}],
            include_assistant_prefix=True,
            has_vision=False
        )

        logger.info(f"Formatted summary prompt for LLM:\n{formatted_prompt}")

        # Set role as user and pass the fully formatted content
        CommonUtils.copy_py_string_to_c_array(
            llm_service.ffi, query.message.role, "user", QUERY_CONST.ROLE_MAX_SIZE
        )
        CommonUtils.copy_py_string_to_c_array(
            llm_service.ffi, query.message.content, formatted_prompt, QUERY_CONST.MESSAGE_CONTENT_MAX_SIZE
        )

        # Set max tokens for summary
        CommonUtils.copy_py_int_to_c_field(llm_service.ffi, query, 'max_completion_tokens', max_summary_tokens)

        # Use lower temperature for more deterministic summary
        CommonUtils.copy_py_float_to_c_field(llm_service.ffi, query, 'temperature', 0.3)

        # Execute inference for summary (accumulate tokens)
        accumulated_tokens = []
        import threading
        completion_event = threading.Event()

        @llm_service.ffi.callback("void(const Response *)")
        def callback(response_ptr):
            try:
                resp = response_ptr[0]
                choice = resp.choices[0]
                msg = choice.message
                content = llm_service.ffi.string(msg.content).decode("utf-8")
                finish_reason = llm_service.ffi.string(choice.finish_reason).decode("utf-8")

                if content:
                    accumulated_tokens.append(content)

                if finish_reason == "stop":
                    completion_event.set()
            except Exception as e:
                logger.error(f"Callback error in summary generation: {e}")
                completion_event.set()

        llm_service.lib.llm_chat_completion_create(
            self.llm_handle, query, False, callback
        )

        # Wait for completion (with timeout)
        if not completion_event.wait(timeout=60):
            logger.error(f"Event {self.event_id}: Summary generation timed out")

        summary_text = "".join(accumulated_tokens)
        summary_tokens = TokenCounter.estimate_tokens(summary_text)

        logger.info(f"Generated summary: {len(summary_text)} chars, {summary_tokens} tokens")

        return summary_text, summary_tokens

    def complete_turn(self):
        """
        Override complete_turn to prevent automatic handle termination in ADHOC_MODE.

        LLM handles should stay cached and only be destroyed when:
        1. Switching to a different LLM model (handled in LLMService.get_or_create_handle)
        2. Explicit cleanup is requested

        This is similar to VLM handle management for consistency.
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

            # IMPORTANT: Do NOT terminate LLM handle in ADHOC_MODE
            # LLM handles are cached and reused across requests
            # They are only destroyed when switching to a different LLM model
            logger.debug(f"Event {self.event_id}: LLM handle kept alive for reuse (ADHOC_MODE)")

            # Trigger completion callback asynchronously (for ADHOC_MODE lock management)
            if self._completion_callback:
                asyncio.create_task(self._trigger_completion_callback())
