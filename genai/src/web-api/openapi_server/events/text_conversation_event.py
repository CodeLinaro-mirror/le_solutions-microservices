# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
TextConversationEvent: Concrete implementation for text-only LLM events.
Handles one complete turn with tool calling and summarization support.
Delegates inference to LLMProcessManager.
"""

import time
import json
import asyncio
from typing import Optional, Dict, Any
from fastapi.responses import StreamingResponse

from openapi_server.events.conversation_event import (
    ConversationEvent,
    EventType,
    EventState
)
from openapi_server.impl.genie_wrapper.llm_process_manager import LLMProcessManager
from openapi_server.events.text_event_helpers import TextEventHelpers
from openapi_server.session.token_counter import TokenCounter
from openapi_server.session.tool_handler import ToolHandler
from openapi_server.managers.model_config_manager import ModelConfigManager
from openapi_server.managers.metrics_manager import MetricsManager
from openapi_server.impl.constant import LLMServiceQueryConstant as QUERY_CONST
from openapi_server.utils.common_utils import CommonUtils
from openapi_server.logger.logger_config import LoggerConfig

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)


class TextConversationEvent(ConversationEvent):
    """
    Text conversation event representing ONE complete turn.
    Handles LLM inference, tool calling, and summarization.
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

        # Retry configuration
        self.retry_count = 0
        self.max_retries = 2

        logger.info(f"Created TextConversationEvent {event_id} for model {model_id} (context: {self.context_size})")

    def _should_summarize(self, projected_tokens: int) -> bool:
        """Determine if summarization should be triggered."""
        # Skip for tool continuations
        if self._is_tool_calling and self._tool_response_received:
            return False

        # Get threshold from model config
        config_manager = ModelConfigManager()
        summary_threshold_ratio = config_manager.get_summarization_threshold(self.model_id)
        threshold = self.context_size * summary_threshold_ratio

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
        """Estimate tokens for the new message."""
        last_msg = request_data.messages[-1]
        content = last_msg.content
        return TokenCounter.estimate_tokens_for_multimodal_content(content)

    async def execute_turn(self, request_data) -> dict:
        """
        Execute this turn (async) using LLMProcessManager.
        May return tool_calls (turn not complete yet) or final response.
        """
        try:
            # 1. Summarization Check
            if not self._is_tool_calling and not self._tool_response_received:
                projected_tokens = self._calculate_projected_tokens(request_data)

                if self._should_summarize(projected_tokens):
                    max_summary_tokens = int(self.context_size * 0.1)
                    logger.info(f"Event {self.event_id}: Triggering summarization (max {max_summary_tokens} tokens)")

                    try:
                        summary_text, summary_tokens = await self.generate_summary(
                            max_summary_tokens,
                            include_history_in_prompt=False
                        )

                        self.session.summary_content = summary_text
                        self.session.summary_token_count = summary_tokens
                        self.session.total_cumulative_tokens += summary_tokens
                        self.summarization_performed = True
                        self.summary_tokens = summary_tokens
                        self.inject_summary = True

                        # Reset the handle's state so it drops the old KV cache
                        # and starts fresh with the summary + new prompt
                        self._reset_handle()

                        logger.info(f"Event {self.event_id}: Summarization complete, {summary_tokens} tokens")
                    except Exception as e:
                        logger.error(f"Event {self.event_id}: Summarization failed: {e}")

            # 2. Build Prompt
            prompt_content = TextEventHelpers.build_prompt_content(
                event_id=self.event_id,
                model_id=self.model_id,
                session=self.session,
                message_indices=self.message_indices,
                request_data=request_data,
                handle_borrowed=getattr(self, 'handle_borrowed', False),
                inject_summary=self.inject_summary,
                is_tool_calling=self._is_tool_calling,
                tool_response_received=self._tool_response_received,
                include_tools=True
            )

            # 3. Execute Inference
            llm_manager = LLMProcessManager.get_instance()
            is_streaming = getattr(request_data, 'stream', False)

            if is_streaming and not self._is_tool_calling and not self._tool_response_received:
                return await self._execute_streaming_inference(llm_manager, prompt_content, request_data)

            # Accumulate non-streaming response
            accumulated_content = []
            non_stream_start = time.time()
            async for token in llm_manager.execute_request(
                event_id=self.event_id,
                session_id=self.session.session_id,
                model=self.model_id,
                prompt=prompt_content,
                streaming=False,
                max_tokens=request_data.max_completion_tokens or QUERY_CONST.DEFAULT_MAX_COMPLETION_TOKENS,
                temperature=request_data.temperature or QUERY_CONST.DEFAULT_TEMPERATURE,
                top_p=request_data.top_p or QUERY_CONST.DEFAULT_TOP_P,
                top_k=getattr(request_data, 'top_k', None),
                presence_penalty=request_data.presence_penalty or QUERY_CONST.DEFAULT_PRESENCE_PENALTY,
                frequency_penalty=request_data.frequency_penalty or QUERY_CONST.DEFAULT_FREQUENCY_PENALTY
            ):
                accumulated_content.append(token)

            response_content = "".join(accumulated_content)

            # Record non-streaming metrics (TPS approximated as total_tokens / total_time)
            try:
                if accumulated_content:
                    MetricsManager.get_instance().record_inference_metrics(
                        model_id=self.model_id,
                        total_pipeline_latency_ms=(time.time() - non_stream_start) * 1000,
                        tokens_generated=len(accumulated_content),
                    )
            except Exception as metrics_err:
                logger.error(f"Event {self.event_id}: Failed to record non-streaming metrics: {metrics_err}")

            if not response_content or response_content.strip() == "":
                raise ValueError("LLM returned empty response")

            # 4. Handle Response (Tool calls vs Regular)
            tool_calls = ToolHandler.parse_tool_response(response_content)

            if tool_calls:
                self._is_tool_calling = True
                self._pending_tool_calls = tool_calls
                logger.info(f"Event {self.event_id}: Tool calling initiated")

                return {
                    "response": tool_calls,
                    "finish_reason": "tool_calls",
                    "needs_tool_response": True,
                    "turn_complete": False,
                }
            else:
                self.assistant_message = response_content
                logger.info(f"Event {self.event_id}: Turn completed successfully")

                return {
                    "response": response_content,
                    "finish_reason": "stop",
                    "needs_tool_response": False,
                    "turn_complete": True,
                }

        except Exception as e:
            # If cancelled, don't retry — just mark as cancelled and return
            if self.is_cancelled:
                logger.info(f"Event {self.event_id}: Non-streaming request cancelled")
                self.cancel_turn()
                return {
                    "response": None,
                    "finish_reason": "cancelled",
                    "needs_tool_response": False,
                    "turn_complete": False,
                }
            return await self._handle_error(e, request_data)

    async def _execute_streaming_inference(self, llm_manager: LLMProcessManager, prompt_content: str, request_data) -> dict:
        """Execute streaming inference with tool detection."""

        async def stream_generator():
            created_time = int(time.time())
            stream_start_time = time.time()
            ttft_timestamp = None          # Time of first token
            last_token_timestamp = None    # For inter-token latency
            inter_token_latencies = []     # Collect per-token latencies

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

            completion_tokens = 0
            full_response_content = []

            has_tools = hasattr(request_data, 'tools') and request_data.tools
            is_potential_tool_call = False
            tool_check_buffer = []
            tool_check_completed = not has_tools

            try:
                async for token in llm_manager.execute_request(
                    event_id=self.event_id,
                    session_id=self.session.session_id,
                    model=self.model_id,
                    prompt=prompt_content,
                    streaming=True,
                    max_tokens=request_data.max_completion_tokens or QUERY_CONST.DEFAULT_MAX_COMPLETION_TOKENS,
                    temperature=request_data.temperature or QUERY_CONST.DEFAULT_TEMPERATURE,
                    top_p=request_data.top_p or QUERY_CONST.DEFAULT_TOP_P,
                    top_k=getattr(request_data, 'top_k', None),
                    presence_penalty=request_data.presence_penalty or QUERY_CONST.DEFAULT_PRESENCE_PENALTY,
                    frequency_penalty=request_data.frequency_penalty or QUERY_CONST.DEFAULT_FREQUENCY_PENALTY
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
                    full_response_content.append(token)

                    # Buffering logic to detect tool calls during streaming
                    if not tool_check_completed:
                        tool_check_buffer.append(token)
                        current_text = "".join(tool_check_buffer).lstrip()

                        if current_text:
                            if current_text.startswith('{'):
                                is_potential_tool_call = True
                                tool_check_completed = True
                                logger.info(f"Event {self.event_id}: Potential tool call detected in stream")
                            elif len(current_text) > 20:
                                is_potential_tool_call = False
                                tool_check_completed = True
                                # Flush buffer
                                for buf_token in tool_check_buffer:
                                    chunk = {
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
                                    yield f"data: {json.dumps(chunk)}\n\n"
                                tool_check_buffer = []
                        continue

                    if is_potential_tool_call:
                        continue
                    else:
                        # Regular content streaming
                        chunk = {
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
                        yield f"data: {json.dumps(chunk)}\n\n"

                # Stream complete
                final_response = "".join(full_response_content)
                parsed_tool_calls = None if not is_potential_tool_call else ToolHandler.parse_tool_response(final_response)

                if parsed_tool_calls:
                    # Valid tool call
                    self._is_tool_calling = True
                    self._pending_tool_calls = parsed_tool_calls

                    # Send tool calls data
                    tc_chunk = {
                        "id": self.session.session_id,
                        "object": "chat.completion.chunk",
                        "created": created_time,
                        "model": self.model_id,
                        "choices": [{
                            "index": 0,
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
                                    } for idx, tc in enumerate(parsed_tool_calls)
                                ]
                            },
                            "finish_reason": None,
                            "logprobs": None
                        }]
                    }
                    yield f"data: {json.dumps(tc_chunk)}\n\n"

                    finish_chunk = {
                        "id": self.session.session_id,
                        "object": "chat.completion.chunk",
                        "created": created_time,
                        "model": self.model_id,
                        "choices": [{
                            "index": 0,
                            "delta": {},
                            "finish_reason": "tool_calls",
                            "logprobs": None
                        }]
                    }
                    yield f"data: {json.dumps(finish_chunk)}\n\n"

                    # Register in tool map (don't complete event yet)
                    from openapi_server.session.conversation_utils import ConversationUtils
                    from openapi_server.managers.session_manager import SessionManager

                    user_message_indices = [idx for idx in self.message_indices if self.session.messages[idx].get('role') == 'user']
                    user_messages = [self.session.messages[idx] for idx in user_message_indices]
                    event_hash = ConversationUtils.calculate_hash_for_specific_messages(user_messages)
                    SessionManager.get_instance().register_tool_calling_event(event_hash, self.session.session_id)

                else:
                    # Regular content or failed tool parse
                    if is_potential_tool_call:
                        # Flush buffered content
                        chunk_size = 100
                        for i in range(0, len(final_response), chunk_size):
                            chunk = {
                                "id": self.session.session_id,
                                "object": "chat.completion.chunk",
                                "created": created_time,
                                "model": self.model_id,
                                "choices": [{
                                    "index": 0,
                                    "delta": {"content": final_response[i:i+chunk_size]},
                                    "finish_reason": None,
                                    "logprobs": None
                                }]
                            }
                            yield f"data: {json.dumps(chunk)}\n\n"

                    # Final stop chunk
                    final_chunk = {
                        "id": self.session.session_id,
                        "object": "chat.completion.chunk",
                        "created": created_time,
                        "model": self.model_id,
                        "choices": [{
                            "index": 0,
                            "delta": {},
                            "finish_reason": "stop",
                            "logprobs": None
                        }]
                    }
                    yield f"data: {json.dumps(final_chunk)}\n\n"

                    # Complete event
                    self.assistant_message = final_response
                    assistant_idx = self.session.add_message({'role': 'assistant', 'content': final_response})
                    self.message_indices.append(assistant_idx)

                    self.complete_turn()
                    self.calculate_event_hash()
                    self.session.complete_current_event()

                yield "data: [DONE]\n\n"

                if self._completion_callback:
                    await self._completion_callback(self.event_id, self.state)

            except Exception as e:
                # Check if this error is due to cancellation
                if self.is_cancelled or "closed file" in str(e).lower() or isinstance(e, (EOFError, BrokenPipeError)):
                    logger.info(f"Event {self.event_id}: Stream terminated due to cancellation")
                    # Don't yield error chunk for cancelled requests
                else:
                    logger.error(f"Error in stream generator: {e}", exc_info=True)
                    # Record failure for health monitoring (not for cancellations)
                    try:
                        MetricsManager.get_instance().record_inference_failure(self.model_id)
                    except Exception:
                        pass
                    self.terminate_handle(force=True)

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
                            model_id=self.model_id,
                            total_pipeline_latency_ms=total_pipeline_latency_ms,
                            tokens_generated=completion_tokens,
                            ttft_ms=ttft_ms,
                            avg_stream_latency_ms=avg_stream_latency_ms,
                        )
                        logger.debug(
                            f"Event {self.event_id}: LLM metrics — "
                            f"TTFT={ttft_ms:.1f}ms, "
                            f"StreamLatency={avg_stream_latency_ms:.1f}ms, "
                            f"Total={total_pipeline_latency_ms:.1f}ms, "
                            f"Tokens={completion_tokens}"
                        )
                except Exception as metrics_err:
                    logger.error(f"Event {self.event_id}: Failed to record metrics: {metrics_err}")

                # Always ensure event is completed/cancelled/failed and callback triggered
                if self.state == EventState.ACTIVE:
                    if self.is_cancelled:
                        logger.info(f"Event {self.event_id}: Stream ended due to cancellation")
                        self.cancel_turn()
                    else:
                        logger.warning(f"Event {self.event_id}: Stream ended without completion, marking as failed")
                        self.fail_turn(Exception("Stream aborted or failed"))

                if self._completion_callback:
                    logger.info(f"Event {self.event_id}: Triggering completion callback in finally block")
                    await self._completion_callback(self.event_id, self.state)

        return {
            "response": StreamingResponse(stream_generator(), media_type="text/event-stream", headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"}),
            "finish_reason": "stop",
            "needs_tool_response": False,
            "turn_complete": True,
            "is_streaming": True
        }

    async def continue_with_tool_response(self, tool_response: str, request_data) -> dict:
        """Continue turn after receiving tool response."""
        try:
            if not self._is_tool_calling:
                raise RuntimeError(f"Event {self.event_id}: Not in tool calling state")

            self.tool_info = tool_response
            self._tool_response_received = True

            logger.info(f"Event {self.event_id}: Generating final answer after tool call")

            # Build Prompt
            import copy
            context_messages = copy.deepcopy(self.session.get_event_messages(self))

            if hasattr(request_data, 'tools') and request_data.tools:
                if any(m.get('role') == 'system' for m in context_messages):
                    context_messages = ToolHandler.inject_tool_instructions(context_messages, request_data.tools)

            # Filter existing tools and append new result
            prompt_messages = [msg for msg in context_messages if msg.get('role', '') != 'tool']
            prompt_messages.append({
                "role": "user",
                "content": f"{tool_response}\n\nBased on the tool result above, please provide a helpful response."
            })

            formatted_content = CommonUtils.build_chat_prompt(
                model_id=self.model_id,
                messages=prompt_messages,
                include_assistant_prefix=True,
                has_vision=False
            )

            # Execute Inference
            llm_manager = LLMProcessManager.get_instance()
            accumulated_content = []

            async for token in llm_manager.execute_request(
                event_id=self.event_id,
                session_id=self.session.session_id,
                model=self.model_id,
                prompt=formatted_content,
                streaming=False,
                max_tokens=request_data.max_completion_tokens or QUERY_CONST.DEFAULT_MAX_COMPLETION_TOKENS,
                temperature=request_data.temperature or QUERY_CONST.DEFAULT_TEMPERATURE,
                top_p=request_data.top_p or QUERY_CONST.DEFAULT_TOP_P,
                top_k=getattr(request_data, 'top_k', None),
                presence_penalty=request_data.presence_penalty or QUERY_CONST.DEFAULT_PRESENCE_PENALTY,
                frequency_penalty=request_data.frequency_penalty or QUERY_CONST.DEFAULT_FREQUENCY_PENALTY
            ):
                accumulated_content.append(token)

            final_response = "".join(accumulated_content)

            if not final_response or final_response.strip() == "":
                raise ValueError("LLM returned empty response after tool call")

            self.assistant_message = final_response
            self._is_tool_calling = False

            logger.info(f"Event {self.event_id}: Turn completed after tool calling")

            return {
                "response": final_response,
                "finish_reason": "stop",
                "needs_tool_response": False,
                "turn_complete": True
            }

        except Exception as e:
            return await self._handle_error(e, request_data)

    async def generate_summary(self, max_summary_tokens: int, messages_to_summarize: Optional[list] = None, include_history_in_prompt: bool = True) -> tuple:
        """Generate a summary of the conversation using LLMProcessManager."""

        source_messages = messages_to_summarize if messages_to_summarize is not None else self.session.messages
        relevant_messages = [msg for msg in source_messages if msg.get('role', '') != 'tool']

        if len(relevant_messages) > 4:
            relevant_messages = relevant_messages[-4:]

        context_parts = []
        for msg in relevant_messages:
            role = msg.get('role', '')
            content = msg.get('content', '')

            content_text = content if isinstance(content, str) else ' '.join(i.get('text', '') for i in content if isinstance(i, dict) and i.get('type') == 'text')
            tool_call_text = " [Tool Calls]" if msg.get('tool_calls') else ""

            if content_text or tool_call_text:
                context_parts.append(f"{role.capitalize()}: {content_text}{tool_call_text}")

        summary_prompt = (
            f"Below is a conversation that needs to be summarized:\n\n{chr(10).join(context_parts)}\n\n"
            f"Please provide a concise summary of the conversation above. The summary should be no more than {max_summary_tokens} tokens. "
            "Focus on the key points and maintain chronological order."
        ) if include_history_in_prompt else (
            f"Please provide a concise summary of the conversation above. The summary should be no more than {max_summary_tokens} tokens. Focus on the key points."
        )

        formatted_prompt = CommonUtils.build_chat_prompt(
            model_id=self.model_id,
            messages=[{"role": "user", "content": summary_prompt}],
            include_assistant_prefix=True,
            has_vision=False
        )

        llm_manager = LLMProcessManager.get_instance()
        accumulated_tokens = []

        async for token in llm_manager.execute_request(
            event_id=f"{self.event_id}-summary",
            session_id=self.session.session_id,
            model=self.model_id,
            prompt=formatted_prompt,
            streaming=False,
            max_tokens=max_summary_tokens,
            temperature=0.3
        ):
            accumulated_tokens.append(token)

        summary_text = "".join(accumulated_tokens)
        summary_tokens = TokenCounter.estimate_tokens(summary_text)

        logger.info(f"Generated summary: {len(summary_text)} chars, {summary_tokens} tokens")
        return summary_text, summary_tokens

    async def _handle_error(self, error: Exception, request_data) -> dict:
        """Handle error with retry logic."""
        self.retry_count += 1
        logger.error(f"Event {self.event_id}: Error (attempt {self.retry_count}/{self.max_retries}): {error}")

        if isinstance(error, (TimeoutError, ConnectionError, ValueError)) and self.retry_count < self.max_retries:
            logger.info(f"Event {self.event_id}: Retrying...")
            try:
                if self._tool_response_received:
                    return await self.continue_with_tool_response(self.tool_info, request_data)
                else:
                    return await self.execute_turn(request_data)
            except Exception as retry_error:
                logger.error(f"Event {self.event_id}: Retry failed: {retry_error}")

        self.terminate_handle(force=True)
        self.fail_turn(error)
        return {
            "response": None,
            "finish_reason": "error",
            "needs_tool_response": False,
            "turn_complete": False,
            "error": {"type": type(error).__name__, "message": str(error)}
        }

    # Handle management
    def take_over_handle(self, previous_event: ConversationEvent): pass
    def create_new_handle(self): pass
    def release_handle(self): pass

    def _reset_handle(self):
        """Forcefully reset the handle's KV cache via process manager."""
        try:
            llm_manager = LLMProcessManager.get_instance()
            if hasattr(llm_manager, '_send_reset_and_wait'):
                logger.info(f"Event {self.event_id}: Resetting LLM handle KV cache")
                llm_manager._send_reset_and_wait()
        except Exception as e:
            logger.error(f"Event {self.event_id}: Error resetting LLM handle: {e}")

    def terminate_handle(self, force: bool = False):
        """
        Terminate the handle/process.

        Args:
            force: If True, force kill subprocess immediately (for cancellation)
        """
        try:
            LLMProcessManager.get_instance().shutdown(force=force)
        except Exception as e:
            logger.error(f"Event {self.event_id}: Error terminating LLM process: {e}")

    def complete_turn(self):
        """Complete the turn."""
        if self.state == EventState.ACTIVE:
            self.state = EventState.COMPLETED
            self.completed_at = time.time()
            self.calculate_token_usage()
            self.calculate_turn_hash()

            logger.info(f"Event {self.event_id}: Turn COMPLETED, "
                       f"prompt: {self.prompt_tokens}, "
                       f"completion: {self.completion_tokens}, "
                       f"total: {self.total_turn_tokens} tokens")

            if self._completion_callback:
                asyncio.create_task(self._trigger_completion_callback())

    def _calculate_prompt_tokens(self) -> int:
        tokens = 0
        for idx in self.message_indices:
            if idx < len(self.session.messages):
                msg = self.session.messages[idx]
                if msg.get('role') == 'user':
                    tokens += 4 + TokenCounter.estimate_tokens(msg.get('content', ''))
        return tokens

    def _calculate_completion_tokens(self) -> int:
        tokens = 0
        for idx in self.message_indices:
            if idx < len(self.session.messages):
                msg = self.session.messages[idx]
                role = msg.get('role', '')
                if role == 'assistant':
                    if msg.get('content'):
                        tokens += TokenCounter.estimate_tokens(msg.get('content', ''))
                    if msg.get('tool_calls'):
                        tool_calls = msg.get('tool_calls', [])
                        serializable_tool_calls = [
                            tc.model_dump() if hasattr(tc, 'model_dump')
                            else tc.dict() if hasattr(tc, 'dict')
                            else tc
                            for tc in tool_calls
                        ]
                        tokens += TokenCounter.estimate_tokens(json.dumps(serializable_tool_calls))
                elif role == 'tool':
                    if msg.get('content'):
                        tokens += TokenCounter.estimate_tokens(msg.get('content', ''))
                    tokens += 4
        return tokens
