# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
Helper functions for TextConversationEvent to build prompts.
Extracted to simplify the main event class.
"""

from typing import Optional, Dict, Any, List
from openapi_server.utils.common_utils import CommonUtils
from openapi_server.session.tool_handler import ToolHandler
from openapi_server.logger.logger_config import LoggerConfig

logger = LoggerConfig.get_logger(__name__)


class TextEventHelpers:
    """Helper methods for text conversation event prompt building."""

    @staticmethod
    def build_prompt_content(
        event_id: str,
        model_id: str,
        session,
        message_indices: List[int],
        request_data,
        handle_borrowed: bool = False,
        inject_summary: bool = False,
        is_tool_calling: bool = False,
        tool_response_received: bool = False,
        include_tools: bool = True
    ) -> str:
        """
        Build the prompt content for LLM inference.

        Args:
            event_id: Event identifier
            model_id: Model identifier
            session: Conversation session
            message_indices: Indices of messages for this turn
            request_data: Request data with messages and parameters
            handle_borrowed: Whether handle was borrowed from previous event
            inject_summary: Whether to inject summary after reset
            is_tool_calling: Whether currently in tool calling state
            tool_response_received: Whether tool response was received
            include_tools: Whether to include tool instructions

        Returns:
            Formatted prompt string ready for LLM
        """
        from openapi_server.impl.constant import ADHOC_MODE

        previous_context = ""
        # Check if we need to inject previous assistant response (if previous event involved tool calling)
        # This reinforces context even if KV cache is present (not applicable in ADHOC_MODE)
        if handle_borrowed and session.events and not ADHOC_MODE:
            try:
                last_event = session.events[-1]
                # Only if it's the same model
                if last_event.model_id == model_id:
                    last_event_msgs = session.get_event_messages(last_event)
                    # Check if last event had tool calls
                    has_tool_calls = any(m.get('tool_calls') for m in last_event_msgs if m.get('role') == 'assistant')

                    if has_tool_calls:
                        # Get the final assistant response
                        final_assistant_msg = None
                        for m in reversed(last_event_msgs):
                            if m.get('role') == 'assistant' and not m.get('tool_calls'):
                                final_assistant_msg = m
                                break

                        if final_assistant_msg and final_assistant_msg.get('content'):
                            previous_context = final_assistant_msg.get('content')
                            logger.info(f"Event {event_id}: Found previous context to inject: {previous_context[:50]}...")
            except Exception as e:
                logger.warning(f"Failed to check/inject previous tool response: {e}")

        # Get messages for this turn
        messages_to_format = []
        if message_indices:
            import copy
            for idx in message_indices:
                messages_to_format.append(copy.deepcopy(session.messages[idx]))
        else:
            msg_obj = request_data.messages[-1]
            messages_to_format.append({
                "role": getattr(msg_obj, "role", "user"),
                "content": getattr(msg_obj, "content", "")
            })

        # Build context based on mode
        if ADHOC_MODE:
            # ADHOC MODE: Build full conversation history in prompt
            # KV cache is reset before each turn, so we must provide all context via prompt text
            messages_to_format = TextEventHelpers._build_context_for_adhoc_mode(
                event_id, model_id, session, messages_to_format, is_tool_calling, tool_response_received
            )
            logger.info(f"Event {event_id}: ADHOC_MODE - built prompt with {len(messages_to_format)} messages (full history)")
        else:
            # NON-ADHOC MODE: Rely on persistent KV cache
            # Only send new messages since KV cache has previous context

            if inject_summary or not handle_borrowed:
                # First turn or after reset (summarization): Include system prompt + summary + new messages
                logger.info(f"Event {event_id}: Non-ADHOC mode - rebuilding context after reset/first turn")

                rebuilt_messages = []

                # 1. System Prompt (always include on first turn or after reset)
                system_msg = TextEventHelpers._find_system_prompt(session)
                if system_msg:
                    rebuilt_messages.append(system_msg)

                # 2. Summary (if available after reset)
                if inject_summary and hasattr(session, 'summary_content') and session.summary_content:
                    summary_msg = {
                        "role": "system",
                        "content": f"Previous conversation summary:\n{session.summary_content}"
                    }
                    rebuilt_messages.append(summary_msg)

                # 3. Current Turn Messages
                rebuilt_messages.extend(messages_to_format)

                messages_to_format = rebuilt_messages
                logger.info(f"Event {event_id}: Non-ADHOC mode - using {len(messages_to_format)} messages (system + summary + new)")
            else:
                # Subsequent turns with borrowed handle: Only send new messages
                # KV cache already has system prompt and previous conversation
                logger.info(f"Event {event_id}: Non-ADHOC mode - using {len(messages_to_format)} messages (new only, relying on KV cache)")

        # Inject tools
        if include_tools and hasattr(request_data, 'tools') and request_data.tools:
            messages_to_format = ToolHandler.inject_tool_instructions(
                messages_to_format, request_data.tools
            )

        # Prepend previous context to the first user message (not applicable in ADHOC_MODE)
        if previous_context:
            for msg in messages_to_format:
                if isinstance(msg, dict) and msg.get('role') == 'user':
                    original_content = msg.get('content', '')
                    msg['content'] = f"{previous_context} {original_content}"
                    logger.info(f"Event {event_id}: Prepended previous context to user message")
                    break

        # Determine if we should add system prompt
        # Add it if:
        # 1. This is the first turn (no previous events)
        # 2. We are in ADHOC_MODE (full reset every time)
        # 3. We are injecting summary (effectively a reset)
        is_first_turn = (not session.events)
        should_add_system = is_first_turn or ADHOC_MODE or inject_summary

        if not should_add_system:
            logger.info(f"Event {event_id}: Suppressing system prompt (not first turn/reset)")

        # Format messages using unified builder
        formatted_content = CommonUtils.build_chat_prompt(
            model_id=model_id,
            messages=messages_to_format,
            include_assistant_prefix=True,
            has_vision=False,
            add_system_prompt=should_add_system
        )

        return formatted_content

    @staticmethod
    def _find_system_prompt(session) -> Optional[Dict[str, Any]]:
        """Find the most recent system prompt from conversation history."""
        if not session.events:
            return None

        # Iterate backwards through events
        for event in reversed(session.events):
            event_msgs = session.get_event_messages(event)
            for msg in event_msgs:
                if msg.get('role') == 'system':
                    return msg
        return None

    @staticmethod
    def _build_context_for_adhoc_mode(
        event_id: str,
        model_id: str,
        session,
        current_turn_messages: list,
        is_tool_calling: bool,
        tool_response_received: bool
    ) -> list:
        """Build conversation context for ADHOC_MODE."""
        from openapi_server.session.token_counter import TokenCounter
        from openapi_server.managers.model_config_manager import ModelConfigManager

        # Skip context building during tool calling continuation
        if is_tool_calling and tool_response_received:
            logger.info(f"Event {event_id}: Skipping context building (tool continuation)")
            return current_turn_messages

        # Get context size directly from model config
        config_manager = ModelConfigManager()
        context_size = config_manager.get_context_size(model_id)

        # Calculate context budget: 40% for input, 60% for output
        # Reduced to 40% to leave more room to maneuver and avoid "Context Size Exceeded" errors
        max_input_tokens = int(context_size * 0.4)

        # 1. Identify Priority Components
        priority_messages = []

        # System Prompt
        system_message = TextEventHelpers._find_system_prompt(session)
        if system_message:
            priority_messages.append(system_message)

        # Summary
        if session.summary_content:
            summary_msg = {
                "role": "system",
                "content": f"Here is a summary of the conversation so far:\n{session.summary_content}"
            }
            priority_messages.append(summary_msg)

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
            logger.warning(f"Event {event_id}: Priority components use all context budget. Dropping history.")
            return priority_messages

        logger.info(f"Event {event_id}: ADHOC_MODE context building - "
                   f"max_input: {max_input_tokens}, priority: {priority_tokens}, history_budget: {history_budget}")

        # Find the last summarization event to avoid duplicating information
        last_summary_event = session.get_last_summarization_event()

        # Build context from completed events AFTER last summarization (work backwards)
        # Optimization: If we have a summary, it already contains everything before the summarization point,
        # so we only need to include messages from events that occurred AFTER that point.
        # This avoids redundancy and makes better use of the context window.
        history_messages = []
        accumulated_tokens = 0
        events_included = 0

        for event in reversed(session.events):
            # Stop if we've reached the summarization point
            if last_summary_event and event.event_id == last_summary_event.event_id:
                logger.info(f"Event {event_id}: Reached last summarization point at event {event.event_id}, stopping history collection")
                break

            event_messages = session.get_event_messages(event)

            # Filter messages - keep user/assistant, EXCLUDE system
            filtered_messages = []
            for msg in event_messages:
                role = msg.get('role', '')
                if role in ['user', 'assistant']:
                    # Skip assistant messages that only have tool_calls (no content)
                    if role == 'assistant':
                        has_content = msg.get('content') is not None and msg.get('content') != ''
                        has_only_tool_calls = msg.get('tool_calls') and not has_content
                        if has_only_tool_calls:
                            continue
                    filtered_messages.append(msg)

            if not filtered_messages:
                continue

            # Estimate tokens for this event
            if hasattr(event, 'total_turn_tokens') and event.total_turn_tokens > 0:
                event_tokens = event.total_turn_tokens
            else:
                event_tokens = sum(
                    TokenCounter.estimate_tokens_for_multimodal_content(msg.get('content', ''))
                    for msg in filtered_messages
                )

            # Check if adding this event would exceed budget
            if accumulated_tokens + event_tokens > history_budget:
                logger.info(f"Event {event_id}: Stopping context building - would exceed budget")
                break

            # Add event messages to history (prepend since going backwards)
            history_messages = filtered_messages + history_messages
            accumulated_tokens += event_tokens
            events_included += 1

        # Combine: [System] + [Summary] + [History] + [Current Turn]
        final_context = []
        if system_message:
            final_context.append(system_message)
        if session.summary_content:
            final_context.append({
                "role": "system",
                "content": f"Here is a summary of the conversation so far:\n{session.summary_content}"
            })

        final_context.extend(history_messages)
        final_context.extend(current_turn_messages)

        logger.info(f"Event {event_id}: ADHOC_MODE context built - "
                   f"{len(history_messages)} historical messages from {events_included} events, "
                   f"{len(final_context)} total messages")

        return final_context
