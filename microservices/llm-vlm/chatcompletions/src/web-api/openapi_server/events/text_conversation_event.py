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
from fastapi import HTTPException
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
from openapi_server.impl.constant import (
    LLMServiceQueryConstant as QUERY_CONST,
    TOOL_RESPONSE_TIMEOUT_SECONDS,
)
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

        # Default max completion tokens when the client does not specify.
        # Capped at 50% of context_size when the client requests a larger value.
        from openapi_server.impl.constant import DEFAULT_MAX_COMPLETION_TOKENS
        self.default_max_completion_tokens = DEFAULT_MAX_COMPLETION_TOKENS

        # Flag to track if we need to inject summary
        self.inject_summary = False
        self.force_rebuild = False

        # Retry configuration
        self.retry_count = 0
        self.max_retries = 2

        # Exact tokens sent to the model for cap accounting.
        # This is intentionally separate from prompt_tokens/completion_tokens,
        # which remain the legacy usage fields used elsewhere in the service.
        self._cap_prompt_tokens_total = 0
        self._cap_completion_tokens_total = 0
        self._cap_started_from_clean_kv = False

        logger.info(f"Created TextConversationEvent {event_id} for model {model_id} (context: {self.context_size})")

    def _is_rebuild_path(self) -> bool:
        """
        Return True when the next execute_request() will run on a clean KV cache
        and therefore must treat the prompt as self-contained.

        With universal reset (KV cache reset after every inference), every new
        turn starts from a clean KV cache. The only exception is Trip 2 of a
        tool continuation, which uses the live KV cache from Trip 1.
        """
        # Trip 2 tool continuation: KV cache from Trip 1 is still live
        if self._is_tool_calling and not self._tool_response_received:
            return False
        # All other cases: always rebuild from clean KV cache
        return True

    def _cap_cache_usage_tokens(self, started_from_clean_kv: Optional[bool] = None) -> float:
        """
        Tokens already resident in the live KV cache before the next
        execute_request() call.
        """
        if started_from_clean_kv is None:
            started_from_clean_kv = self._is_rebuild_path()

        if started_from_clean_kv:
            return 0

        cached = 0.0
        for event in reversed(self.session.events):
            cached += getattr(event, "_cap_prompt_tokens_total", getattr(event, "prompt_tokens", 0))
            cached += getattr(event, "_cap_completion_tokens_total", getattr(event, "completion_tokens", 0))

            if getattr(event, "_cap_started_from_clean_kv", False):
                break

        if self._is_tool_calling:
            cached += self._cap_prompt_tokens_total + self._cap_completion_tokens_total

        return cached

    def _check_user_query_length(
        self,
        current_turn_messages: list,
        requested_max_completion: Optional[int],
    ) -> None:
        """
        Pre-assembly guard: reject immediately if the user's current turn messages
        are too large to fit in the available input budget, given the current
        session memory state (facts, summary, system prompt).

        This check fires BEFORE prompt assembly so the client gets an immediate
        HTTP 400 with a precise, actionable message — including exactly how many
        tokens their message is and how many tokens are available — without
        touching the inference subprocess.

        The error message includes a breakdown of what is consuming the input
        budget (system prompt, conversation memory, summary, output reservation)
        so the user understands why the limit is what it is.

        This is particularly important for RAG use cases where large document
        chunks are included in the user message.

        Args:
            current_turn_messages:    Messages belonging to the current turn.
            requested_max_completion: Caller's requested max_completion_tokens.
        """
        from openapi_server.impl.constant import (
            HttpStatusCodes,
            ErrorMessages,
            MAX_COMPLETION_SAFETY_MARGIN,
            MIN_USEFUL_COMPLETION_TOKENS,
            ERROR_CODE_CONTEXT_LENGTH_EXCEEDED,
        )

        # Estimate current turn token cost (exclude system messages — handled separately)
        current_turn_tokens = sum(
            TokenCounter.estimate_tokens_for_multimodal_content(msg.get('content', ''))
            + 4  # role/separator overhead per message
            for msg in current_turn_messages
            if msg.get('role') != 'system'
        )
        if current_turn_tokens == 0:
            return

        # Compute output reservation
        output_reserve = int(min(
            requested_max_completion or self.default_max_completion_tokens,
            self.context_size * 0.5,
        ))
        input_budget = self.context_size - output_reserve - MAX_COMPLETION_SAFETY_MARGIN

        # Estimate tokens consumed by fixed slots (system prompt, facts, summary)
        system_tokens = TokenCounter.estimate_tokens(
            getattr(self.session, 'system_prompt_content', None) or ""
        )
        facts_text = self.session.format_facts() if hasattr(self.session, 'format_facts') else ""
        facts_tokens = TokenCounter.estimate_tokens(facts_text) if facts_text else 0
        summary_tokens = TokenCounter.estimate_tokens(
            getattr(self.session, 'summary_content', None) or ""
        )

        # Add per-message overhead for each fixed slot that is present
        fixed_overhead = 0
        if system_tokens > 0:
            fixed_overhead += system_tokens + 4
        if facts_tokens > 0:
            fixed_overhead += facts_tokens + 4
        if summary_tokens > 0:
            fixed_overhead += summary_tokens + 4

        # Maximum tokens available for the user's current turn message
        max_query_tokens = max(0, input_budget - fixed_overhead - MIN_USEFUL_COMPLETION_TOKENS)

        if current_turn_tokens > max_query_tokens:
            raise HTTPException(
                status_code=HttpStatusCodes.BAD_REQUEST,
                detail={
                    "message": ErrorMessages.USER_QUERY_TOO_LONG.format(
                        query_tokens=current_turn_tokens,
                        max_query_tokens=max_query_tokens,
                        context_size=self.context_size,
                        output_tokens=output_reserve,
                    ),
                    "type": "invalid_request_error",
                    "code": ERROR_CODE_CONTEXT_LENGTH_EXCEEDED,
                    "param": "messages",
                },
            )

    def _resolve_max_completion_tokens(self, request_data, prompt_content: str):
        """
        Validate max_completion_tokens against the computed cap. Raises 400 on
        over-cap or prompt-too-long; falls back to min(default, cap) when
        omitted or zero. Also returns the prompt token estimate and rebuild
        decision so callers can reuse them after a successful inference.
        """
        from openapi_server.impl.constant import (
            HttpStatusCodes,
            ErrorMessages,
            MIN_USEFUL_COMPLETION_TOKENS,
            MAX_COMPLETION_SAFETY_MARGIN,
            TOKEN_ESTIMATION_BUFFER_RATIO,
            ERROR_CODE_CONTEXT_LENGTH_EXCEEDED,
        )

        started_from_clean_kv = self._is_rebuild_path()
        prompt_tokens = TokenCounter.estimate_tokens(prompt_content or "")
        cached_context_tokens = self._cap_cache_usage_tokens(started_from_clean_kv)

        # Add a configurable estimation buffer (default 10%) to account for the
        # discrepancy between the character-based heuristic and the actual tokenizer.
        # Without this buffer, prompts that are slightly over the context window may
        # pass the check (because the heuristic underestimates for dense content such
        # as code, JSON, or non-English text) and cause silent truncation.
        estimation_buffer = int(prompt_tokens * TOKEN_ESTIMATION_BUFFER_RATIO)

        cap = max(int(
            self.context_size - cached_context_tokens - prompt_tokens
            - MAX_COMPLETION_SAFETY_MARGIN - estimation_buffer
        ), 0)

        logger.info(
            f"Event {self.event_id}: max_completion_tokens cap - "
            f"cached_context={cached_context_tokens:.0f}, prompt_tokens={prompt_tokens}, "
            f"safety_margin={MAX_COMPLETION_SAFETY_MARGIN}, cap={cap}"
        )

        requested = getattr(request_data, "max_completion_tokens", None)
        if requested is not None and requested < 0:
            raise HTTPException(
                status_code=HttpStatusCodes.BAD_REQUEST,
                detail={
                    "message": "max_completion_tokens must be a non-negative integer.",
                    "type": "invalid_request_error",
                    "code": "invalid_value",
                    "param": "max_completion_tokens",
                },
            )
        if requested == 0:
            requested = None

        # Trip 2 hint: when the tool output is at least half of the prompt,
        # point the error message at that cause.
        tool_response_tokens = TokenCounter.estimate_tokens(self.tool_info) if self.tool_info else 0
        tool_response_dominates = (
            tool_response_tokens > 0
            and tool_response_tokens >= max(prompt_tokens - tool_response_tokens, 0)
        )

        if cap < MIN_USEFUL_COMPLETION_TOKENS:
            template = (
                ErrorMessages.PROMPT_TOO_LONG_TOOL_RESPONSE
                if tool_response_dominates
                else ErrorMessages.PROMPT_TOO_LONG
            )
            raise HTTPException(
                status_code=HttpStatusCodes.BAD_REQUEST,
                detail={
                    "message": template.format(context_size=self.context_size, cap=cap),
                    "type": "invalid_request_error",
                    "code": ERROR_CODE_CONTEXT_LENGTH_EXCEEDED,
                    "param": "messages",
                },
            )

        if requested is not None and requested > cap:
            template = (
                ErrorMessages.CONTEXT_LENGTH_EXCEEDED_TOOL_RESPONSE
                if tool_response_dominates
                else ErrorMessages.CONTEXT_LENGTH_EXCEEDED
            )
            raise HTTPException(
                status_code=HttpStatusCodes.BAD_REQUEST,
                detail={
                    "message": template.format(
                        requested=requested, cap=cap, context_size=self.context_size,
                    ),
                    "type": "invalid_request_error",
                    "code": ERROR_CODE_CONTEXT_LENGTH_EXCEEDED,
                    "param": "max_completion_tokens",
                },
            )

        if requested is not None:
            return requested, prompt_tokens, started_from_clean_kv
        return min(self.default_max_completion_tokens, cap), prompt_tokens, started_from_clean_kv

    def _should_summarize(self, max_completion_tokens: int = 0, current_turn_tokens: int = 0) -> bool:
        """
        Determine if summarization should be triggered using new SDK 2.45+ formula.

        New Formula: (system_prompt_tokens * 1.3) + tokens_so_far + max_completion_tokens > 0.9 * context_size

        This accounts for:
        - System prompt overhead (1.3x multiplier for formatting)
        - Cumulative tokens accumulated so far
        - Reserved space for model completion
        - 90% threshold (more aggressive than old 70%)

        Args:
            max_completion_tokens: Maximum tokens the model may generate in this turn.
            current_turn_tokens: Current event conversational tokens not yet
                reflected in completed session totals.
        """
        # Skip for tool continuations
        if self._is_tool_calling and self._tool_response_received:
            return False

        # Get system prompt tokens (with overhead for formatting)
        from openapi_server.impl.constant import (
            SUMMARIZATION_SYSTEM_PROMPT_OVERHEAD,
            SUMMARIZATION_CONTEXT_THRESHOLD,
            SUMMARIZATION_MAX_COMPLETION_MULTIPLIER
        )

        system_tokens = self.session.system_prompt_tokens or 0
        system_overhead = system_tokens * SUMMARIZATION_SYSTEM_PROMPT_OVERHEAD

        # Get summary tokens (if any)
        summary_tokens = self.session.summary_token_count or 0

        # Get tokens accumulated since last summarization
        # This avoids counting tokens that were already summarized
        tokens_since_last_summary = self.session.calculate_tokens_since_last_summarization()

        # Calculate projected total using new formula
        # Apply multiplier to max_completion_tokens
        projected_total = (
            system_overhead +
            summary_tokens +
            tokens_since_last_summary +
            current_turn_tokens +
            (max_completion_tokens * SUMMARIZATION_MAX_COMPLETION_MULTIPLIER)
        )

        # Use configured context threshold
        threshold = self.context_size * SUMMARIZATION_CONTEXT_THRESHOLD

        should_summarize = projected_total >= threshold

        if should_summarize:
            weighted_max_completion = max_completion_tokens * SUMMARIZATION_MAX_COMPLETION_MULTIPLIER
            logger.info(
                f"Event {self.event_id}: Summarization threshold reached (new formula):\n"
                f"  System overhead: {system_overhead:.0f} tokens ({system_tokens} * {SUMMARIZATION_SYSTEM_PROMPT_OVERHEAD})\n"
                f"  Summary tokens: {summary_tokens}\n"
                f"  Tokens since last summary: {tokens_since_last_summary}\n"
                f"  Current turn conversation: {current_turn_tokens}\n"
                f"  Max completion (weighted): {weighted_max_completion:.0f} tokens ({max_completion_tokens} * {SUMMARIZATION_MAX_COMPLETION_MULTIPLIER})\n"
                f"  Total projected: {projected_total:.0f}\n"
                f"  Threshold ({SUMMARIZATION_CONTEXT_THRESHOLD * 100:.0f}%): {threshold:.0f}"
            )

        return should_summarize

    async def execute_turn(self, request_data) -> dict:
        """
        Execute this turn (async) using LLMProcessManager.
        May return tool_calls (turn not complete yet) or final response.
        """
        try:
            from openapi_server.impl.constant import ADHOC_MODE

            requested_max_completion = getattr(request_data, "max_completion_tokens", None)
            if requested_max_completion is not None and requested_max_completion < 0:
                from openapi_server.impl.constant import HttpStatusCodes
                raise HTTPException(
                    status_code=HttpStatusCodes.BAD_REQUEST,
                    detail={
                        "message": "max_completion_tokens must be a non-negative integer.",
                        "type": "invalid_request_error",
                        "code": "invalid_value",
                        "param": "max_completion_tokens",
                    },
                )
            if requested_max_completion == 0:
                requested_max_completion = None
            if requested_max_completion is not None and requested_max_completion >= self.context_size:
                from openapi_server.impl.constant import (
                    ErrorMessages,
                    ERROR_CODE_CONTEXT_LENGTH_EXCEEDED,
                    HttpStatusCodes,
                )
                raise HTTPException(
                    status_code=HttpStatusCodes.BAD_REQUEST,
                    detail={
                        "message": ErrorMessages.CONTEXT_LENGTH_EXCEEDED.format(
                            requested=requested_max_completion,
                            cap=self.context_size - 1,
                            context_size=self.context_size,
                        ),
                        "type": "invalid_request_error",
                        "code": ERROR_CODE_CONTEXT_LENGTH_EXCEEDED,
                        "param": "max_completion_tokens",
                    },
                )

            adhoc_summary_messages = None
            current_turn_tokens = 0
            if not self._is_tool_calling and not self._tool_response_received:
                import copy

                current_event_messages = [
                    copy.deepcopy(self.session.messages[idx])
                    for idx in self.message_indices
                    if idx < len(self.session.messages)
                ]
                current_conversation_messages = []
                for msg in current_event_messages:
                    role = msg.get('role', '')
                    if role not in ['user', 'assistant']:
                        continue
                    if role == 'assistant':
                        has_content = msg.get('content') not in (None, '')
                        has_only_tool_calls = msg.get('tool_calls') and not has_content
                        if has_only_tool_calls:
                            continue
                    current_conversation_messages.append(msg)
                current_turn_tokens = sum(
                    TokenCounter.estimate_tokens_for_multimodal_content(msg.get('content', ''))
                    for msg in current_conversation_messages
                )
                if ADHOC_MODE:
                    prior_session_messages = []
                    current_message_indices = set(self.message_indices)
                    for idx, msg in enumerate(self.session.messages):
                        if idx in current_message_indices:
                            continue
                        role = msg.get('role', '')
                        if role not in ['user', 'assistant']:
                            continue
                        if role == 'assistant':
                            has_content = msg.get('content') not in (None, '')
                            has_only_tool_calls = msg.get('tool_calls') and not has_content
                            if has_only_tool_calls:
                                continue
                        prior_session_messages.append(copy.deepcopy(msg))
                    if prior_session_messages:
                        adhoc_summary_messages = prior_session_messages

                # Pre-assembly user query length guard.
                # Reject immediately if the current turn messages are too large
                # to fit in the available input budget given the current session
                # memory state (facts, summary, system prompt). This fires before
                # prompt assembly and subprocess interaction, giving the client a
                # precise HTTP 400 with the exact token limit and a breakdown of
                # what is consuming the budget.
                self._check_user_query_length(
                    current_conversation_messages,
                    requested_max_completion,
                )

            # 1. Summarization Check
            if not self._is_tool_calling and not self._tool_response_received:
                max_completion_target = requested_max_completion or self.default_max_completion_tokens
                should_summarize = self._should_summarize(
                    max_completion_target,
                    current_turn_tokens=current_turn_tokens,
                )
                if should_summarize:
                    from openapi_server.impl.constant import SUMMARIZATION_SUMMARY_SIZE_RATIO
                    max_summary_tokens = int(self.context_size * SUMMARIZATION_SUMMARY_SIZE_RATIO)
                    logger.info(f"Event {self.event_id}: Triggering summarization (max {max_summary_tokens} tokens)")

                    if ADHOC_MODE and not adhoc_summary_messages:
                        logger.info(
                            f"Event {self.event_id}: Skipping ADHOC summarization - "
                            "no older history beyond the live request tail"
                        )
                    else:
                        try:
                            if ADHOC_MODE:
                                summary_text, summary_tokens = await self.generate_summary(
                                    max_summary_tokens,
                                    messages_to_summarize=adhoc_summary_messages,
                                    include_history_in_prompt=True
                                )
                            else:
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

                            # Summarization only compresses older completed
                            # history; the live request remains verbatim in the
                            # rebuilt prompt.
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
                rebuild_with_history=self.force_rebuild,
                is_tool_calling=self._is_tool_calling,
                tool_response_received=self._tool_response_received,
                include_tools=True
            )
            # 3. Execute Inference
            max_completion, prompt_tokens, started_from_clean_kv = self._resolve_max_completion_tokens(
                request_data,
                prompt_content,
            )
            llm_manager = LLMProcessManager.get_instance()
            is_streaming = getattr(request_data, 'stream', False)

            if is_streaming and not self._is_tool_calling and not self._tool_response_received:
                return await self._execute_streaming_inference(
                    llm_manager,
                    prompt_content,
                    request_data,
                    max_completion,
                    prompt_tokens,
                    started_from_clean_kv,
                )

            # Accumulate non-streaming response
            accumulated_content = []
            non_stream_start = time.time()
            ttft_timestamp = None
            last_token_timestamp = None
            inter_token_latencies = []

            async for token in llm_manager.execute_request(
                event_id=self.event_id,
                session_id=self.session.session_id,
                model=self.model_id,
                prompt=prompt_content,
                streaming=False,
                max_tokens=max_completion,
                temperature=request_data.temperature or QUERY_CONST.DEFAULT_TEMPERATURE,
                top_p=request_data.top_p or QUERY_CONST.DEFAULT_TOP_P,
                top_k=getattr(request_data, 'top_k', QUERY_CONST.DEFAULT_TOP_K),
                presence_penalty=request_data.presence_penalty or QUERY_CONST.DEFAULT_PRESENCE_PENALTY,
                frequency_penalty=request_data.frequency_penalty or QUERY_CONST.DEFAULT_FREQUENCY_PENALTY
            ):
                now = time.time()
                if ttft_timestamp is None:
                    ttft_timestamp = now
                if last_token_timestamp is not None:
                    inter_token_latencies.append((now - last_token_timestamp) * 1000)
                last_token_timestamp = now
                accumulated_content.append(token)

            response_content = "".join(accumulated_content)

            if not response_content or response_content.strip() == "":
                raise ValueError("LLM returned empty response")

            self._cap_started_from_clean_kv = started_from_clean_kv
            self._cap_prompt_tokens_total += prompt_tokens
            self._cap_completion_tokens_total += TokenCounter.estimate_tokens(response_content or "")

            # Record non-streaming metrics only for successful completions.
            try:
                if accumulated_content:
                    ttft_ms = (ttft_timestamp - non_stream_start) * 1000 if ttft_timestamp else None
                    avg_stream_latency_ms = (
                        sum(inter_token_latencies) / len(inter_token_latencies)
                        if inter_token_latencies else None
                    )
                    MetricsManager.get_instance().record_inference_metrics(
                        model_id=self.model_id,
                        total_pipeline_latency_ms=(time.time() - non_stream_start) * 1000,
                        tokens_generated=TokenCounter.estimate_tokens(response_content),
                        ttft_ms=ttft_ms,
                        avg_stream_latency_ms=avg_stream_latency_ms,
                    )
            except Exception as metrics_err:
                logger.error(f"Event {self.event_id}: Failed to record non-streaming metrics: {metrics_err}")

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

                # Post-turn processing for non-streaming complete turns.
                # Reset KV cache after inference, then run eviction/summarization/facts.
                # This runs synchronously here (before returning) so the session is
                # fully prepared for the next turn before the DSP lock is released.
                self._reset_handle()
                try:
                    await self._post_turn_processing()
                except Exception as _ptp_err:
                    logger.error(
                        f"Event {self.event_id}: post-turn processing error (non-streaming): {_ptp_err}"
                    )

                return {
                    "response": response_content,
                    "finish_reason": "stop",
                    "needs_tool_response": False,
                    "turn_complete": True,
                }

        except asyncio.CancelledError:
            logger.info(f"Event {self.event_id}: Non-streaming request cancelled by client")
            self.is_cancelled = True
            if self.state == EventState.ACTIVE:
                try:
                    self.terminate_handle(force=True)
                except Exception as cancel_err:
                    logger.error(f"Event {self.event_id}: Error terminating handle on cancel: {cancel_err}")
                self.cancel_turn()
            raise
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

    async def _execute_streaming_inference(
        self,
        llm_manager: LLMProcessManager,
        prompt_content: str,
        request_data,
        max_completion: int,
        prompt_tokens: int,
        started_from_clean_kv: bool,
    ) -> dict:
        """Execute streaming inference with tool detection, decoupled from network I/O via asyncio.Queue."""

        token_queue: asyncio.Queue = asyncio.Queue()
        _DONE_SENTINEL = object()

        async def _inference_producer():
            """
            Runs LLM inference and puts pre-formatted SSE chunks into token_queue.
            Releases the DSP lock (via _completion_callback) as soon as inference
            completes, independent of client network speed.
            """
            created_time = int(time.time())
            stream_start_time = time.time()
            ttft_timestamp = None
            last_token_timestamp = None
            inter_token_latencies = []

            completion_tokens = 0
            full_response_content = []
            stream_outcome = "pending"
            stream_failure: Optional[Exception] = None

            has_tools = hasattr(request_data, 'tools') and request_data.tools
            is_potential_tool_call = False
            tool_check_buffer = []
            tool_check_completed = not has_tools

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
            await token_queue.put(f"data: {json.dumps(first_chunk)}\n\n")

            try:
                async for token in llm_manager.execute_request(
                    event_id=self.event_id,
                    session_id=self.session.session_id,
                    model=self.model_id,
                    prompt=prompt_content,
                    streaming=True,
                    max_tokens=max_completion,
                    temperature=request_data.temperature or QUERY_CONST.DEFAULT_TEMPERATURE,
                    top_p=request_data.top_p or QUERY_CONST.DEFAULT_TOP_P,
                    top_k=getattr(request_data, 'top_k', QUERY_CONST.DEFAULT_TOP_K),
                    presence_penalty=request_data.presence_penalty or QUERY_CONST.DEFAULT_PRESENCE_PENALTY,
                    frequency_penalty=request_data.frequency_penalty or QUERY_CONST.DEFAULT_FREQUENCY_PENALTY
                ):
                    now = time.time()
                    if ttft_timestamp is None:
                        ttft_timestamp = now
                    if last_token_timestamp is not None:
                        inter_token_latencies.append((now - last_token_timestamp) * 1000)
                    last_token_timestamp = now

                    # Notify watchdog
                    from openapi_server.impl.constant import ADHOC_MODE
                    if ADHOC_MODE:
                        try:
                            from openapi_server.managers.request_queue_manager import RequestQueueManager
                            RequestQueueManager.get_instance().update_inference_heartbeat()
                        except Exception:
                            pass

                    completion_tokens += 1
                    full_response_content.append(token)

                    # Tool call detection buffering
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
                                for buf_token in tool_check_buffer:
                                    chunk = {
                                        "id": self.session.session_id,
                                        "object": "chat.completion.chunk",
                                        "created": created_time,
                                        "model": self.model_id,
                                        "choices": [{"index": 0, "delta": {"content": buf_token}, "finish_reason": None, "logprobs": None}]
                                    }
                                    await token_queue.put(f"data: {json.dumps(chunk)}\n\n")
                                tool_check_buffer = []
                        continue

                    if is_potential_tool_call:
                        continue
                    else:
                        chunk = {
                            "id": self.session.session_id,
                            "object": "chat.completion.chunk",
                            "created": created_time,
                            "model": self.model_id,
                            "choices": [{"index": 0, "delta": {"content": token}, "finish_reason": None, "logprobs": None}]
                        }
                        await token_queue.put(f"data: {json.dumps(chunk)}\n\n")

                # Inference complete
                final_response = "".join(full_response_content)
                self._cap_started_from_clean_kv = started_from_clean_kv
                self._cap_prompt_tokens_total += prompt_tokens
                self._cap_completion_tokens_total += TokenCounter.estimate_tokens(final_response or "")
                parsed_tool_calls = None if not is_potential_tool_call else ToolHandler.parse_tool_response(final_response)

                if parsed_tool_calls:
                    self._is_tool_calling = True
                    self._pending_tool_calls = parsed_tool_calls

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
                                        "function": {"name": tc.function.name, "arguments": tc.function.arguments}
                                    } for idx, tc in enumerate(parsed_tool_calls)
                                ]
                            },
                            "finish_reason": None,
                            "logprobs": None
                        }]
                    }
                    await token_queue.put(f"data: {json.dumps(tc_chunk)}\n\n")

                    finish_chunk = {
                        "id": self.session.session_id,
                        "object": "chat.completion.chunk",
                        "created": created_time,
                        "model": self.model_id,
                        "choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls", "logprobs": None}]
                    }
                    await token_queue.put(f"data: {json.dumps(finish_chunk)}\n\n")

                    from openapi_server.session.conversation_utils import ConversationUtils
                    from openapi_server.managers.session_manager import SessionManager
                    user_message_indices = [idx for idx in self.message_indices if self.session.messages[idx].get('role') == 'user']
                    user_messages = [self.session.messages[idx] for idx in user_message_indices]
                    event_hash = ConversationUtils.calculate_hash_for_specific_messages(user_messages)
                    SessionManager.get_instance().register_tool_calling_event(event_hash, self.session.session_id)
                    self.start_tool_response_timeout(TOOL_RESPONSE_TIMEOUT_SECONDS)

                else:
                    if is_potential_tool_call:
                        chunk_size = 100
                        for i in range(0, len(final_response), chunk_size):
                            chunk = {
                                "id": self.session.session_id,
                                "object": "chat.completion.chunk",
                                "created": created_time,
                                "model": self.model_id,
                                "choices": [{"index": 0, "delta": {"content": final_response[i:i+chunk_size]}, "finish_reason": None, "logprobs": None}]
                            }
                            await token_queue.put(f"data: {json.dumps(chunk)}\n\n")
                    elif tool_check_buffer:
                        for buf_token in tool_check_buffer:
                            chunk = {
                                "id": self.session.session_id,
                                "object": "chat.completion.chunk",
                                "created": created_time,
                                "model": self.model_id,
                                "choices": [{"index": 0, "delta": {"content": buf_token}, "finish_reason": None, "logprobs": None}]
                            }
                            await token_queue.put(f"data: {json.dumps(chunk)}\n\n")
                        tool_check_buffer = []

                    final_chunk = {
                        "id": self.session.session_id,
                        "object": "chat.completion.chunk",
                        "created": created_time,
                        "model": self.model_id,
                        "choices": [{"index": 0, "delta": {}, "finish_reason": "stop", "logprobs": None}]
                    }
                    await token_queue.put(f"data: {json.dumps(final_chunk)}\n\n")

                    self.assistant_message = final_response
                    assistant_idx = self.session.add_message({'role': 'assistant', 'content': final_response})
                    self.message_indices.append(assistant_idx)
                    self.complete_turn()
                    self.calculate_event_hash()
                    self.session.complete_current_event()

                await token_queue.put("data: [DONE]\n\n")
                stream_outcome = "success"

            except asyncio.CancelledError:
                stream_outcome = "cancelled"
                logger.info(f"Event {self.event_id}: Stream cancelled by client")
                self.is_cancelled = True
                if self.state == EventState.ACTIVE:
                    try:
                        self.terminate_handle(force=True)
                    except Exception as cancel_err:
                        logger.error(f"Event {self.event_id}: Error terminating handle on cancel: {cancel_err}")
                else:
                    logger.info(f"Event {self.event_id}: Client cancelled after inference completed (state={self.state.name}) — not killing subprocess")
                raise

            except Exception as e:
                if self.is_cancelled:
                    stream_outcome = "cancelled"
                    logger.info(f"Event {self.event_id}: Stream terminated due to cancellation")
                else:
                    stream_outcome = "failure"
                    stream_failure = e
                    logger.error(f"Error in inference producer: {e}", exc_info=True)

                    from openapi_server.impl.constant import GenieErrorMappings
                    error_msg = str(e)
                    layman_msg = GenieErrorMappings.get_layman_message(error_msg)
                    status_code = GenieErrorMappings.get_http_status_code(error_msg, default_status=500)
                    final_msg = layman_msg if layman_msg else error_msg
                    for prefix in ("LLM subprocess error: ", "VLM subprocess error: "):
                        if final_msg.startswith(prefix):
                            final_msg = final_msg[len(prefix):]
                            break

                    error_payload = {
                        "error": {
                            "message": final_msg,
                            "type": "server_error",
                            "param": None,
                            "code": status_code
                        }
                    }
                    try:
                        token_queue.put_nowait(f"data: {json.dumps(error_payload)}\n\n")
                        token_queue.put_nowait("data: [DONE]\n\n")
                    except Exception:
                        pass

            finally:
                # Record metrics
                try:
                    if stream_outcome == "success" and completion_tokens > 0:
                        total_pipeline_latency_ms = (time.time() - stream_start_time) * 1000
                        ttft_ms = (ttft_timestamp - stream_start_time) * 1000 if ttft_timestamp else None
                        avg_stream_latency_ms = (
                            sum(inter_token_latencies) / len(inter_token_latencies)
                            if inter_token_latencies else None
                        )
                        MetricsManager.get_instance().record_inference_metrics(
                            model_id=self.model_id,
                            total_pipeline_latency_ms=total_pipeline_latency_ms,
                            tokens_generated=TokenCounter.estimate_tokens("".join(full_response_content)),
                            ttft_ms=ttft_ms,
                            avg_stream_latency_ms=avg_stream_latency_ms,
                        )
                    elif stream_outcome == "failure":
                        MetricsManager.get_instance().record_inference_failure(self.model_id)
                except Exception as metrics_err:
                    logger.error(f"Event {self.event_id}: Failed to record metrics: {metrics_err}")

                # Ensure event state is set
                if self.state == EventState.ACTIVE:
                    if stream_outcome == "cancelled" or self.is_cancelled:
                        logger.info(f"Event {self.event_id}: Stream ended due to cancellation")
                        self.cancel_turn()
                    elif stream_outcome == "success" and self._is_tool_calling and not self._tool_response_received:
                        logger.info(f"Event {self.event_id}: Stream ended after tool call request; keeping event ACTIVE for tool continuation")
                    else:
                        logger.warning(f"Event {self.event_id}: Stream ended without completion, marking as failed")
                        if self.state == EventState.ACTIVE:
                            try:
                                self.terminate_handle(force=True)
                            except Exception as _term_err:
                                logger.error(f"Event {self.event_id}: Error terminating handle on unexpected exit: {_term_err}")
                        self.fail_turn(stream_failure or Exception("Stream aborted or failed"))

                # ── Post-turn processing (while DSP lock is still held) ────────
                # Run eviction, summarization, and fact extraction BEFORE releasing
                # the DSP lock. This ensures the next turn's prompt is fully
                # prepared (summary + facts updated) before the lock is released.
                # Only runs for complete turns (not tool call Trip 1, not cancelled).
                if (stream_outcome == "success"
                        and not self._is_tool_calling
                        and not self.is_cancelled):
                    # Reset KV cache after user turn inference (universal reset)
                    self._reset_handle()
                    # Run post-turn memory management.
                    # asyncio.shield() prevents client disconnection from
                    # interrupting _post_turn_processing() mid-execution.
                    # Without shielding, a client disconnect cancels the
                    # producer_task while the summary/facts inference is running,
                    # leaving the subprocess in an inconsistent state and causing
                    # the next request to hang on "Waiting for eager background
                    # RESET to complete...".
                    try:
                        await asyncio.shield(self._post_turn_processing())
                    except asyncio.CancelledError:
                        # Client disconnected during post-turn processing.
                        # _post_turn_processing() is shielded and will complete
                        # independently. Suppress the CancelledError so the
                        # finally block can fire the completion callback cleanly.
                        logger.info(
                            f"Event {self.event_id}: client disconnected during "
                            "post-turn processing — shielded task will complete independently"
                        )
                    except Exception as _ptp_err:
                        logger.error(
                            f"Event {self.event_id}: post-turn processing error: {_ptp_err}"
                        )

                # ── Release DSP lock ──────────────────────────────────────────
                # Fires AFTER post-turn processing so the next request sees
                # updated summary and facts immediately.
                if self._completion_callback:
                    logger.info(f"Event {self.event_id}: Triggering completion callback from inference producer")
                    try:
                        await self._completion_callback(self.event_id, self.state)
                    except asyncio.CancelledError:
                        logger.info(f"Event {self.event_id}: Task cancelled, scheduling callback as background task")
                        try:
                            asyncio.create_task(self._completion_callback(self.event_id, self.state))
                        except Exception as _task_err:
                            logger.error(f"Event {self.event_id}: Error scheduling background callback: {_task_err}")
                    except RuntimeError:
                        try:
                            asyncio.create_task(self._completion_callback(self.event_id, self.state))
                        except Exception as _task_err:
                            logger.error(f"Event {self.event_id}: Error scheduling fallback callback: {_task_err}")
                    except Exception as _cb_err:
                        logger.error(f"Event {self.event_id}: Error in completion callback: {_cb_err}")

                # Signal consumer that inference is done
                try:
                    token_queue.put_nowait(_DONE_SENTINEL)
                except Exception:
                    pass

        # Start inference as a background task, decoupled from network I/O
        producer_task = asyncio.create_task(_inference_producer())

        async def stream_generator():
            """
            Consumes pre-formatted SSE chunks from the queue and yields them to the client.
            Network I/O is completely decoupled from inference speed.
            """
            try:
                while True:
                    try:
                        item = await asyncio.wait_for(token_queue.get(), timeout=0.5)
                    except asyncio.TimeoutError:
                        # Check if producer is done and queue is empty
                        if producer_task.done() and token_queue.empty():
                            break
                        continue

                    if item is _DONE_SENTINEL:
                        break
                    yield item

            except asyncio.CancelledError:
                # Client disconnected - cancel the inference producer if still running
                if not producer_task.done():
                    producer_task.cancel()
                    try:
                        # Don't block network exit waiting for producer to clean up,
                        # it will clean up asynchronously
                        pass
                    except Exception:
                        pass
                raise

            finally:
                # Ensure producer is cleaned up if still running
                if not producer_task.done():
                    producer_task.cancel()

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

            logger.info(f"Event {self.event_id}: Generating final answer after tool call")

            # Build Prompt
            import copy
            context_messages = copy.deepcopy(self.session.get_event_messages(self))
            system_message = TextEventHelpers._build_effective_system_message(
                self.session,
                include_summary=False
            )
            if system_message:
                context_messages.insert(0, system_message)

            if hasattr(request_data, 'tools') and request_data.tools:
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
            max_completion, prompt_tokens, _ = self._resolve_max_completion_tokens(
                request_data,
                formatted_content,
            )
            self.mark_tool_response_received()
            llm_manager = LLMProcessManager.get_instance()
            accumulated_content = []

            async for token in llm_manager.execute_request(
                event_id=self.event_id,
                session_id=self.session.session_id,
                model=self.model_id,
                prompt=formatted_content,
                streaming=False,
                max_tokens=max_completion,
                temperature=request_data.temperature or QUERY_CONST.DEFAULT_TEMPERATURE,
                top_p=request_data.top_p or QUERY_CONST.DEFAULT_TOP_P,
                top_k=getattr(request_data, 'top_k', QUERY_CONST.DEFAULT_TOP_K),
                presence_penalty=request_data.presence_penalty or QUERY_CONST.DEFAULT_PRESENCE_PENALTY,
                frequency_penalty=request_data.frequency_penalty or QUERY_CONST.DEFAULT_FREQUENCY_PENALTY
            ):
                accumulated_content.append(token)

            final_response = "".join(accumulated_content)

            if not final_response or final_response.strip() == "":
                raise ValueError("LLM returned empty response after tool call")

            self._cap_prompt_tokens_total += prompt_tokens
            self._cap_completion_tokens_total += TokenCounter.estimate_tokens(final_response or "")

            self.assistant_message = final_response
            self._is_tool_calling = False

            logger.info(f"Event {self.event_id}: Turn completed after tool calling")

            # Post-turn processing for tool continuation complete turns.
            # Reset KV cache after Trip 2 inference, then run eviction/summarization/facts.
            # This runs before releasing the DSP lock so the next turn is fully prepared.
            self._reset_handle()
            try:
                await self._post_turn_processing()
            except Exception as _ptp_err:
                logger.error(
                    f"Event {self.event_id}: post-turn processing error (tool continuation): {_ptp_err}"
                )

            # Release DSP lock after post-turn processing completes
            if self._completion_callback:
                try:
                    await self._completion_callback(self.event_id, EventState.COMPLETED)
                except Exception as cb_err:
                    logger.error(f"Event {self.event_id}: Error in tool continuation callback: {cb_err}")

            return {
                "response": final_response,
                "finish_reason": "stop",
                "needs_tool_response": False,
                "turn_complete": True
            }

        except asyncio.CancelledError:
            logger.info(f"Event {self.event_id}: Tool continuation request cancelled by client")
            self.is_cancelled = True
            if self.state == EventState.ACTIVE:
                try:
                    self.terminate_handle(force=True)
                except Exception as cancel_err:
                    logger.error(f"Event {self.event_id}: Error terminating handle on cancel: {cancel_err}")
                self.cancel_turn()
            raise
        except Exception as e:
            return await self._handle_error(e, request_data)

    async def generate_summary(
        self,
        max_summary_tokens: int,
        messages_to_summarize: Optional[list] = None,
        include_history_in_prompt: bool = True,
    ) -> tuple:
        """
        Generate a structured summary of the conversation using LLMProcessManager.

        Improvements over the previous implementation:
        - Removed the 4-message cap: summarizes the full eviction batch
        - Uses a structured prompt that preserves facts, tasks, key exchanges,
          and open items — not just a generic narrative recap
        - Prepends prior summary for continuity (accumulated rolling summary)
        - Token-budget-aware: limits input to 50% of context window

        Args:
            max_summary_tokens:      Maximum tokens for the generated summary.
            messages_to_summarize:   Messages to summarize. Defaults to session.messages.
            include_history_in_prompt: Legacy parameter, kept for call-site compat.

        Returns:
            Tuple of (summary_text, summary_token_count).
        """
        from openapi_server.impl.constant import STRUCTURED_SUMMARY_PROMPT

        source_messages = (
            messages_to_summarize
            if messages_to_summarize is not None
            else self.session.messages
        )
        # Exclude tool messages — they are verbose and rarely useful in summaries
        relevant_messages = [
            msg for msg in source_messages if msg.get('role', '') != 'tool'
        ]

        if not relevant_messages:
            return "", 0

        # If a prior summary exists, prepend it so the new summary is a
        # continuation rather than an independent snapshot of the batch.
        if getattr(self.session, 'summary_content', None):
            prior_summary_msg = {
                "role": "system",
                "content": f"[Prior summary: {self.session.summary_content}]",
            }
            relevant_messages = [prior_summary_msg] + relevant_messages

        # Build conversation text, token-budget-aware.
        # Limit input to 50% of context to leave room for the summary prompt itself.
        input_budget = int(self.context_size * 0.5)
        conversation_parts = []
        accumulated = 0
        for msg in relevant_messages:
            role = msg.get('role', '').capitalize()
            content = msg.get('content', '') or ''
            if isinstance(content, list):
                content = ' '.join(
                    i.get('text', '') for i in content
                    if isinstance(i, dict) and i.get('type') == 'text'
                )
            tool_call_text = " [Tool Calls]" if msg.get('tool_calls') else ""
            line = f"{role}: {content}{tool_call_text}"
            line_tokens = TokenCounter.estimate_tokens(line)
            if accumulated + line_tokens > input_budget:
                break
            conversation_parts.append(line)
            accumulated += line_tokens

        if not conversation_parts:
            return "", 0

        conversation_text = "\n".join(conversation_parts)
        summary_prompt = STRUCTURED_SUMMARY_PROMPT.format(
            max_tokens=max_summary_tokens,
            conversation=conversation_text,
        )

        formatted_prompt = CommonUtils.build_chat_prompt(
            model_id=self.model_id,
            messages=[{"role": "user", "content": summary_prompt}],
            include_assistant_prefix=True,
            has_vision=False,
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
            temperature=0.3,
        ):
            accumulated_tokens.append(token)

        summary_text = "".join(accumulated_tokens).strip()
        summary_tokens = TokenCounter.estimate_tokens(summary_text)

        logger.info(
            f"Event {self.event_id}: summary generated — "
            f"{len(relevant_messages)} messages → {summary_tokens} tokens"
        )
        return summary_text, summary_tokens

    async def _extract_facts(self, messages_to_extract: list) -> None:
        """
        Extract persistent facts from a batch of messages using the LLM.

        Called as part of post-turn processing after an eviction batch has been
        summarized. Updates session.facts with newly extracted key-value pairs.

        The LLM is asked to return a JSON object of short string key-value pairs
        representing persistent facts (names, goals, decisions, constraints, etc.).
        New facts override existing ones for the same key; keys are never deleted.

        Args:
            messages_to_extract: Messages to extract facts from (eviction batch).
        """
        import re as _re
        from openapi_server.impl.constant import FACT_EXTRACTION_PROMPT, SLOT_FACTS_CEILING

        relevant = [
            msg for msg in messages_to_extract
            if msg.get('role') in ('user', 'assistant')
        ]
        if not relevant:
            return

        # Build conversation text for the extraction prompt
        conversation_parts = []
        for msg in relevant:
            role = msg.get('role', '').capitalize()
            content = msg.get('content', '') or ''
            if isinstance(content, list):
                content = ' '.join(
                    i.get('text', '') for i in content
                    if isinstance(i, dict) and i.get('type') == 'text'
                )
            if content:
                conversation_parts.append(f"{role}: {content}")

        if not conversation_parts:
            return

        prompt_text = FACT_EXTRACTION_PROMPT.format(
            conversation="\n".join(conversation_parts)
        )

        formatted_prompt = CommonUtils.build_chat_prompt(
            model_id=self.model_id,
            messages=[{"role": "user", "content": prompt_text}],
            include_assistant_prefix=True,
            has_vision=False,
        )

        llm_manager = LLMProcessManager.get_instance()
        tokens = []

        async for token in llm_manager.execute_request(
            event_id=f"{self.event_id}-facts",
            session_id=self.session.session_id,
            model=self.model_id,
            prompt=formatted_prompt,
            streaming=False,
            max_tokens=150,
            temperature=0.0,
        ):
            tokens.append(token)

        raw = "".join(tokens).strip()

        # Parse JSON from the response (model may add surrounding text)
        new_facts = {}
        try:
            new_facts = json.loads(raw)
        except json.JSONDecodeError:
            match = _re.search(r'\{[^{}]*\}', raw, _re.DOTALL)
            if match:
                try:
                    new_facts = json.loads(match.group())
                except json.JSONDecodeError:
                    pass

        if new_facts and isinstance(new_facts, dict):
            turn_number = len(self.session.events)
            self.session.update_facts(new_facts, turn_number)
            self.session.enforce_facts_ceiling(SLOT_FACTS_CEILING)
            logger.info(
                f"Event {self.event_id}: extracted {len(new_facts)} facts "
                f"({len(self.session.facts)} total in store)"
            )
        else:
            logger.debug(f"Event {self.event_id}: no facts extracted from batch")

    async def _post_turn_processing(self) -> None:
        """
        Post-turn memory management: eviction, summarization, and fact extraction.

        Called after every complete turn (not tool continuations) while the DSP
        lock is still held. This ensures the next turn's prompt is fully prepared
        before the lock is released.

        Sequence:
          1. Compute history token usage (includes the just-completed turn)
          2. If over eviction threshold: identify oldest messages to evict
          3. Mark eviction batch in session (excluded from next turn's history queue)
          4. Summarize eviction batch → update session.summary_content
          5. Reset KV cache after summarization inference
          6. Extract facts from eviction batch → update session.facts
          7. Reset KV cache after fact extraction inference
          8. Clear eviction batch (messages now represented by summary)
        """
        from openapi_server.impl.constant import (
            HISTORY_EVICTION_THRESHOLD,
            HISTORY_EVICTION_TARGET,
            MAX_COMPLETION_SAFETY_MARGIN,
        )

        try:
            # Compute slot ceilings proportional to context size so that small-context
            # models (e.g. 2048 tokens) still have a non-zero history budget.
            # Each slot is capped at its absolute ceiling but also at a fraction of
            # the available input budget to prevent fixed overheads from consuming
            # the entire context on small models.
            output_reserve = int(self.context_size * 0.5)
            input_budget = self.context_size - output_reserve - MAX_COMPLETION_SAFETY_MARGIN

            # Slot fractions: system=12%, tools=14%, facts=14%, summary=20% of input budget
            # These fractions sum to 60%, leaving 40% for history.
            slot_system  = min(int(input_budget * 0.12), 256)
            slot_tools   = min(int(input_budget * 0.14), 300)
            slot_facts   = min(int(input_budget * 0.14), 300)
            slot_summary = min(int(input_budget * 0.20), 400)

            fixed_overhead = slot_system + slot_tools + slot_facts + slot_summary
            history_budget = max(input_budget - fixed_overhead, 0)

            logger.debug(
                f"Event {self.event_id}: post-turn budget — "
                f"context={self.context_size}, input={input_budget}, "
                f"fixed={fixed_overhead} (sys={slot_system} tools={slot_tools} "
                f"facts={slot_facts} summary={slot_summary}), "
                f"history={history_budget}"
            )

            # Get all post-summary messages (excludes eviction batch and system msgs)
            history_messages = self.session.get_post_summary_messages()
            if not history_messages:
                return

            current_tokens = sum(
                TokenCounter.estimate_tokens_for_multimodal_content(m.get('content', '')) + 4
                for m in history_messages
            )

            if current_tokens <= history_budget * HISTORY_EVICTION_THRESHOLD:
                logger.debug(
                    f"Event {self.event_id}: post-turn — no eviction needed "
                    f"({current_tokens}/{history_budget} tokens, "
                    f"threshold={HISTORY_EVICTION_THRESHOLD:.0%})"
                )
                return

            # Identify eviction batch: remove oldest messages until under target
            target_tokens = int(history_budget * HISTORY_EVICTION_TARGET)
            eviction_batch = []
            remaining = list(history_messages)

            while remaining and current_tokens > target_tokens:
                msg = remaining.pop(0)
                msg_tokens = (
                    TokenCounter.estimate_tokens_for_multimodal_content(msg.get('content', ''))
                    + 4
                )
                eviction_batch.append(msg)
                current_tokens -= msg_tokens

            if not eviction_batch:
                return

            logger.info(
                f"Event {self.event_id}: post-turn eviction — "
                f"{len(eviction_batch)} messages evicted, "
                f"target={target_tokens} tokens"
            )

            # Mark eviction batch in session so next turn's history queue excludes them
            self.session.eviction_batch = eviction_batch

            # Summarize eviction batch.
            # execute_request() inside generate_summary() handles the KV cache reset
            # via the eager background reset mechanism — no explicit _reset_handle()
            # call is needed here (it would cause a double reset).
            summary_text, summary_tokens = await self.generate_summary(
                max_summary_tokens=slot_summary,
                messages_to_summarize=eviction_batch,
            )
            if summary_text:
                self.session.summary_content = summary_text
                self.session.summary_token_count = summary_tokens
                self.summarization_performed = True
                logger.info(
                    f"Event {self.event_id}: post-turn summary updated "
                    f"({summary_tokens} tokens)"
                )

            # Extract facts from eviction batch.
            # Same: execute_request() inside _extract_facts() handles the reset.
            await self._extract_facts(eviction_batch)

            # Clear eviction batch — messages are now represented by the summary
            self.session.eviction_batch = []

        except Exception as e:
            logger.error(f"Event {self.event_id}: post-turn processing failed: {e}")
            # Always clear eviction batch on failure to avoid stale exclusions
            self.session.eviction_batch = []

    async def _handle_error(self, error: Exception, request_data) -> dict:
        """
        Handle inference error by failing immediately.

        Server-side retry has been removed: retrying on the server keeps the DSP
        lock held longer and causes the request queue to grow when multiple clients
        are waiting. Clients are responsible for their own retry logic.
        """
        if isinstance(error, HTTPException):
            raise error

        logger.error(f"Event {self.event_id}: Error: {error}")

        try:
            MetricsManager.get_instance().record_inference_failure(self.model_id)
        except Exception as metrics_err:
            logger.error(f"Event {self.event_id}: Failed to record terminal inference failure: {metrics_err}")

        # Avoid abrupt global kill for per-request failures.
        # Graceful shutdown lets the manager recover without force-killing the subprocess.
        self.terminate_handle(force=False)
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

            # NOTE: Do NOT trigger the completion callback here.
            # The streaming generator's finally block calls it after the subprocess
            # sends READY, ensuring the DSP is truly idle before the next request.

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
