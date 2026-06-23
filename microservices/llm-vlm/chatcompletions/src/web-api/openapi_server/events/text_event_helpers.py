# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
Helper functions for TextConversationEvent to build prompts.

Implements a unified, slot-based prompt assembly strategy:

  Slot 1 — System block       (256 tokens max): caller's system prompt
  Slot 2 — Tool instructions  (300 tokens max): compact tool schemas (when tools present)
  Slot 3 — Core memory/facts  (300 tokens max): LLM-extracted facts (when available)
  Slot 4 — Episodic summary   (400 tokens max): rolling summary of evicted history
  Slot 5 — History queue      (remainder):      recent turns, per-message, newest-first

Every turn builds a complete, self-contained prompt from a clean KV cache.
The ADHOC/non-ADHOC branching has been removed — both modes use this path.
"""

import copy
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
        # Legacy parameters kept for call-site compatibility — no longer used
        # for branching logic. Both ADHOC and non-ADHOC use the unified path.
        handle_borrowed: bool = False,
        inject_summary: bool = False,
        rebuild_with_history: bool = False,
        is_tool_calling: bool = False,
        tool_response_received: bool = False,
        include_tools: bool = True,
    ) -> str:
        """
        Build the complete, self-contained prompt for LLM inference.

        Every call produces a full prompt starting from a clean KV cache.
        The prompt is assembled from named slots with hard token ceilings.

        Tool continuation (Trip 2) is the only exception: it uses the live
        KV cache from Trip 1 and sends only the tool response context.

        Args:
            event_id:              Event identifier for logging.
            model_id:              Model identifier.
            session:               ConversationSession with shared message history.
            message_indices:       Indices of messages belonging to this turn.
            request_data:          Request data with messages and parameters.
            handle_borrowed:       Legacy — ignored.
            inject_summary:        Legacy — ignored (summary always injected when present).
            rebuild_with_history:  Legacy — ignored (always rebuilds).
            is_tool_calling:       True when in tool calling state (Trip 1 complete).
            tool_response_received: True when tool response received (Trip 2).
            include_tools:         Whether to inject tool instructions.

        Returns:
            Formatted prompt string ready for LLM inference.
        """
        # Extract current turn messages (deep copy to avoid mutating session state)
        current_turn_messages = [
            copy.deepcopy(session.messages[idx])
            for idx in message_indices
            if idx < len(session.messages)
        ]
        # Strip system messages from current turn — handled by slot assembly
        current_turn_body = [
            m for m in current_turn_messages if m.get('role') != 'system'
        ]

        # Tool continuation (Trip 2): use event messages only, no full rebuild
        if is_tool_calling and tool_response_received:
            messages = TextEventHelpers._build_tool_continuation_messages(
                session, request_data, current_turn_body
            )
        else:
            # Standard path: full slot-based context assembly
            messages = TextEventHelpers._build_complete_prompt_context(
                event_id, model_id, session, current_turn_body, request_data
            )

        # Inject tool instructions into the assembled messages
        if include_tools and hasattr(request_data, 'tools') and request_data.tools:
            messages = ToolHandler.inject_tool_instructions(messages, request_data.tools)

        # Determine whether to include the system prompt prefix in the formatted output.
        # For tool continuations, the system prompt was already sent in Trip 1.
        add_system_prompt = not (is_tool_calling and tool_response_received)

        return CommonUtils.build_chat_prompt(
            model_id=model_id,
            messages=messages,
            include_assistant_prefix=True,
            has_vision=False,
            add_system_prompt=add_system_prompt,
        )

    @staticmethod
    def _build_complete_prompt_context(
        event_id: str,
        model_id: str,
        session,
        current_turn_body: list,
        request_data,
    ) -> list:
        """
        Assemble the full prompt context from named slots.

        Slot priority (highest = never evicted):
          1. System block       — caller's system prompt, truncated to ceiling
          2. Tool instructions  — injected later by build_prompt_content()
          3. Core memory/facts  — LLM-extracted facts from session
          4. Episodic summary   — rolling summary of evicted history
          5. History queue      — recent turns, per-message, newest-first

        Args:
            event_id:          Event identifier for logging.
            model_id:          Model identifier.
            session:           ConversationSession.
            current_turn_body: Current turn messages (user/assistant, no system).
            request_data:      Request data for output reserve calculation.

        Returns:
            List of message dicts ready for prompt formatting.
        """
        from openapi_server.session.token_counter import TokenCounter
        from openapi_server.managers.model_config_manager import ModelConfigManager
        from openapi_server.impl.constant import (
            MAX_COMPLETION_SAFETY_MARGIN,
            SLOT_SYSTEM_CEILING,
            SLOT_TOOLS_CEILING,
            SLOT_FACTS_CEILING,
            SLOT_SUMMARY_CEILING,
        )

        config_manager = ModelConfigManager()
        context_size = config_manager.get_context_size(model_id)

        # Dynamic output reserve: use actual requested max_completion_tokens
        # rather than a fixed ratio. Cap at 50% of context to prevent
        # degenerate cases where the caller requests an unreasonably large output.
        requested_output = (
            getattr(request_data, 'max_completion_tokens', None)
            or int(context_size * 0.5)
        )
        output_reserve = min(int(requested_output), int(context_size * 0.5))
        input_budget = context_size - output_reserve - MAX_COMPLETION_SAFETY_MARGIN

        messages = []
        used_tokens = 0

        # ── Slot 1: System block ──────────────────────────────────────────────
        system_content = TextEventHelpers._build_system_content(session, SLOT_SYSTEM_CEILING)
        messages.append({"role": "system", "content": system_content})
        used_tokens += TokenCounter.estimate_tokens(system_content) + 4  # +4 for role overhead

        # ── Slot 3: Core memory (facts) ───────────────────────────────────────
        # Injected before the summary so the model sees stable facts first.
        if hasattr(session, 'format_facts'):
            facts_text = session.format_facts()
            if facts_text:
                facts_tokens = TokenCounter.estimate_tokens(facts_text) + 4
                if facts_tokens <= SLOT_FACTS_CEILING:
                    messages.append({"role": "system", "content": facts_text})
                    used_tokens += facts_tokens

        # ── Slot 4: Episodic summary ──────────────────────────────────────────
        if getattr(session, 'summary_content', None):
            summary_text = f"[Earlier conversation summary: {session.summary_content}]"
            summary_tokens = TokenCounter.estimate_tokens(summary_text) + 4
            if summary_tokens <= SLOT_SUMMARY_CEILING:
                messages.append({"role": "system", "content": summary_text})
                used_tokens += summary_tokens

        # ── Slot 5: History queue ─────────────────────────────────────────────
        # Reserve tokens for current turn overhead (role markers, separators)
        current_turn_tokens = sum(
            TokenCounter.estimate_tokens_for_multimodal_content(m.get('content', '')) + 4
            for m in current_turn_body
        )
        # Also reserve for tool instructions slot (worst case: full ceiling)
        # This prevents the history queue from consuming tokens needed for tools.
        has_tools = hasattr(request_data, 'tools') and bool(request_data.tools)
        tool_reserve = SLOT_TOOLS_CEILING if has_tools else 0

        history_budget = input_budget - used_tokens - current_turn_tokens - tool_reserve
        history_budget = max(0, history_budget)

        history_messages = TextEventHelpers._fill_history_queue(
            session, current_turn_body, history_budget
        )
        messages.extend(history_messages)

        # ── Current turn ──────────────────────────────────────────────────────
        messages.extend(current_turn_body)

        logger.info(
            f"Event {event_id}: prompt assembled — "
            f"context={context_size}, input_budget={input_budget}, "
            f"fixed_slots={used_tokens}, tool_reserve={tool_reserve}, "
            f"history_budget={history_budget}, "
            f"history_msgs={len(history_messages)}, "
            f"current_turn_msgs={len(current_turn_body)}"
        )
        return messages

    @staticmethod
    def _build_system_content(session, ceiling_tokens: int) -> str:
        """
        Build the system block content (Slot 1).

        Uses the session's active system prompt. Truncates to ceiling_tokens
        if the prompt exceeds the budget, appending a [truncated] sentinel.

        Args:
            session:        ConversationSession with system_prompt_content.
            ceiling_tokens: Hard token ceiling for this slot.

        Returns:
            System prompt string, guaranteed within ceiling_tokens.
        """
        from openapi_server.session.token_counter import TokenCounter

        content = getattr(session, 'system_prompt_content', None) or ""
        if not content:
            return "You are a helpful assistant."

        tokens = TokenCounter.estimate_tokens(content)
        if tokens <= ceiling_tokens:
            return content

        # Truncate: ~3 chars per token heuristic
        char_limit = ceiling_tokens * 3
        truncated = content[:char_limit]
        logger.warning(
            f"System prompt truncated from {tokens} to ~{ceiling_tokens} tokens"
        )
        return truncated + " [truncated]"

    @staticmethod
    def _fill_history_queue(
        session,
        current_turn_body: list,
        budget_tokens: int,
    ) -> list:
        """
        Fill the history queue (Slot 5) with recent messages, newest-first,
        up to budget_tokens.

        Excludes:
          - Messages in session.eviction_batch (pending summarization)
          - Messages in current_turn_body (added separately as current turn)
          - System messages (handled by Slot 1)
          - Tool-only assistant messages (no content, only tool_calls)

        Args:
            session:           ConversationSession with message history.
            current_turn_body: Current turn messages to exclude.
            budget_tokens:     Maximum tokens for the history queue.

        Returns:
            List of message dicts, ordered oldest-first (for correct prompt order).
        """
        from openapi_server.session.token_counter import TokenCounter

        if budget_tokens <= 0:
            return []

        # Build exclusion sets using object identity
        eviction_ids = {id(m) for m in getattr(session, 'eviction_batch', [])}
        current_ids = {id(m) for m in current_turn_body}

        # Collect eligible messages in session order
        eligible = []
        for msg in session.messages:
            if id(msg) in eviction_ids or id(msg) in current_ids:
                continue
            role = msg.get('role', '')
            if role == 'system':
                continue
            if role == 'assistant':
                has_content = msg.get('content') not in (None, '')
                if msg.get('tool_calls') and not has_content:
                    continue
            eligible.append(msg)

        # Fill newest-first until budget exhausted
        result = []
        accumulated = 0
        for msg in reversed(eligible):
            msg_tokens = (
                TokenCounter.estimate_tokens_for_multimodal_content(msg.get('content', ''))
                + 4  # role + separator overhead
            )
            if accumulated + msg_tokens > budget_tokens:
                break
            result.insert(0, msg)
            accumulated += msg_tokens

        return result

    @staticmethod
    def _build_tool_continuation_messages(
        session,
        request_data,
        current_turn_body: list,
    ) -> list:
        """
        Build messages for tool continuation (Trip 2).

        Trip 2 uses the live KV cache from Trip 1, so we only need to send
        the tool response and a prompt for the final answer. The full
        conversation history is already in the KV cache.

        Args:
            session:           ConversationSession.
            request_data:      Request data.
            current_turn_body: Current turn messages (tool response + user prompt).

        Returns:
            Minimal message list for tool continuation.
        """
        # Build effective system message (without full history rebuild)
        system_msg = TextEventHelpers._find_system_prompt(session)
        messages = []
        if system_msg:
            messages.append(system_msg)

        # Add current turn messages (tool response)
        messages.extend(current_turn_body)
        return messages

    @staticmethod
    def _find_system_prompt(session) -> Optional[Dict[str, Any]]:
        """Return the active session system prompt as a message dict, if any."""
        content = getattr(session, 'system_prompt_content', None)
        if content:
            return {"role": "system", "content": content}
        return None

    @staticmethod
    def _build_effective_system_message(
        session,
        include_summary: bool = True,
    ) -> Optional[Dict[str, Any]]:
        """
        Build one effective system message combining the active system prompt
        and optional conversation summary.

        Used by tool continuation and model-switch paths.

        Args:
            session:         ConversationSession.
            include_summary: Whether to append the episodic summary.

        Returns:
            System message dict, or None if no content.
        """
        parts = []

        system_msg = TextEventHelpers._find_system_prompt(session)
        if system_msg:
            content = system_msg.get('content', '')
            if content:
                parts.append(content if isinstance(content, str) else str(content))

        if include_summary and getattr(session, 'summary_content', None):
            parts.append(f"Previous conversation summary:\n{session.summary_content}")

        if not parts:
            return None

        return {"role": "system", "content": "\n\n".join(parts)}
