# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
ConversationEvent: Represents a single complete conversation turn.
Each event handles one user message → assistant response cycle.
"""

import time
import hashlib
import asyncio
from enum import Enum
from abc import ABC, abstractmethod
from typing import Optional, Any, List, Dict, Callable
from queue import Queue

from openapi_server.logger.logger_config import LoggerConfig
from openapi_server.session.token_counter import TokenCounter

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)


class EventState(Enum):
    """Event state for a single turn."""
    ACTIVE = "active"        # Processing this turn (may include tool calling)
    COMPLETED = "completed"  # Turn successfully completed
    FAILED = "failed"        # Turn failed
    CANCELLED = "cancelled"  # Turn cancelled


class EventType(Enum):
    """Type of event."""
    TEXT = "text"
    VISION = "vision"


class ConversationTurn:
    """Data class representing a complete conversation turn."""

    def __init__(self, user: str, assistant: Optional[str] = None,
                 tool: Optional[str] = None, model_used: Optional[str] = None):
        self.user = user
        self.assistant = assistant
        self.tool = tool
        self.model_used = model_used
        self.timestamp = time.time()
        self.tokens = 0

    def to_dict(self) -> dict:
        """Convert to dictionary."""
        return {
            "user": self.user,
            "assistant": self.assistant,
            "tool": self.tool,
            "model_used": self.model_used,
            "timestamp": self.timestamp,
            "tokens": self.tokens
        }


class ConversationEvent(ABC):
    """
    Abstract base class representing a SINGLE complete conversation turn.
    Handles one user message → assistant response cycle.
    May include tool calling as part of the turn.

    Events reference messages in the session's shared message history via message_indices.
    """

    def __init__(self,
                 event_id: str,
                 model_id: str,
                 event_type: EventType,
                 session: 'ConversationSession'):
        # Identity
        self.event_id = event_id
        self.session = session  # Reference to parent session
        self.session_id = session.session_id
        self.model_id = model_id
        self.event_type = event_type

        # Message indices into session.messages
        self.message_indices: List[int] = []

        # State
        self.state = EventState.ACTIVE
        self.created_at = time.time()
        self.completed_at: Optional[float] = None
        self.failed_at: Optional[float] = None

        # Legacy fields (for backward compatibility, but data is in session.messages)
        self.user_message: Optional[str] = None
        self.assistant_message: Optional[str] = None
        self.tool_info: Optional[str] = None

        # LLM Handle management
        self.llm_handle: Optional[Any] = None
        self.handle_owned = False  # True if this event created the handle
        self.handle_borrowed = False  # True if borrowed from previous event

        # Enhanced token tracking (for this turn only)
        self.prompt_tokens = 0           # Input tokens for this turn
        self.completion_tokens = 0       # Generated tokens for this turn
        self.total_turn_tokens = 0       # prompt_tokens + completion_tokens
        self.context_size = 0

        # Legacy field (for backward compatibility)
        self.turn_tokens = 0

        # Cumulative token tracking
        self.cumulative_tokens_before = 0    # Session total before this event
        self.cumulative_tokens_after = 0     # Session total after this event

        # Summarization tracking
        self.summarization_performed = False  # True if this event triggered summarization
        self.summary_tokens = 0              # Tokens in summary (if summarization_performed)

        # Hash (for this turn)
        self.turn_hash = ""

        # Event hash (for session matching based on message indices)
        self.event_hash = None

        # Error tracking
        self.error_info: Optional[dict] = None
        self.retry_count = 0
        self.max_retries = 2

        # Retry replay metadata
        self.request_signature: Optional[str] = None
        self.replay_result: Optional[dict] = None

        # Internal state (tool calling)
        self._is_tool_calling = False
        self._pending_tool_calls: List[dict] = []
        self._tool_response_received = False
        self._waiting_for_tool_response = False
        self._tool_wait_started_at: Optional[float] = None
        self._tool_wait_deadline: Optional[float] = None
        self._tool_wait_timed_out = False
        self._tool_timeout_task: Optional[asyncio.Task] = None
        self._tool_timeout_lock = asyncio.Lock()

        # Cancellation flag — set by session.cancel_active_event()
        self.is_cancelled = False

        # Completion callback (for ADHOC_MODE lock management)
        self._completion_callback: Optional[Callable] = None

    def start_tool_response_timeout(self, timeout_seconds: int):
        """
        Start timeout tracking for tool response.
        Keeps event state ACTIVE but marks it as waiting for tool output.
        """
        self.cancel_tool_response_timeout(clear_flags=False)

        now = time.time()
        self._waiting_for_tool_response = True
        self._tool_wait_started_at = now
        self._tool_wait_deadline = now + timeout_seconds
        self._tool_wait_timed_out = False

        self._tool_timeout_task = asyncio.create_task(
            self._tool_response_timeout_worker(timeout_seconds)
        )
        logger.info(
            f"Event {self.event_id}: Waiting for tool response (timeout={timeout_seconds}s)"
        )

    def cancel_tool_response_timeout(self, clear_flags: bool = True):
        """Cancel active tool response timeout task if any."""
        if self._tool_timeout_task and not self._tool_timeout_task.done():
            self._tool_timeout_task.cancel()
        self._tool_timeout_task = None

        if clear_flags:
            self._waiting_for_tool_response = False
            self._tool_wait_started_at = None
            self._tool_wait_deadline = None

    def is_waiting_for_tool_response(self) -> bool:
        """Return True if event is waiting for external tool output."""
        return self._waiting_for_tool_response and self._is_tool_calling and not self._tool_response_received

    def has_tool_response_timed_out(self) -> bool:
        """Return True if waiting window has already expired."""
        if self._tool_wait_timed_out:
            return True
        if not self._tool_wait_deadline:
            return False
        return time.time() > self._tool_wait_deadline

    def mark_tool_response_received(self):
        """Mark that tool response arrived in time and timeout watcher can stop."""
        self._tool_response_received = True
        self.cancel_tool_response_timeout(clear_flags=True)

    @property
    def tool_wait_timed_out(self) -> bool:
        return self._tool_wait_timed_out

    async def _tool_response_timeout_worker(self, timeout_seconds: int):
        """Background task that fails the event if tool output never arrives."""
        try:
            await asyncio.sleep(timeout_seconds)
            await self.expire_tool_wait(
                reason=f"Tool response timed out after {timeout_seconds} seconds"
            )
        except asyncio.CancelledError:
            return

    async def expire_tool_wait(self, reason: str) -> bool:
        """
        Expire current tool-wait window and fail the event.
        Returns True when timeout transition happened, False otherwise.
        """
        async with self._tool_timeout_lock:
            if not self.is_active():
                return False
            if not self._is_tool_calling:
                return False
            if self._tool_response_received:
                return False
            if self._tool_wait_timed_out:
                return False

            self._tool_wait_timed_out = True
            self._waiting_for_tool_response = False
            self._tool_wait_deadline = time.time()
            self._tool_timeout_task = None

            logger.warning(f"Event {self.event_id}: {reason}")

            self.fail_turn(TimeoutError(reason))

            try:
                from openapi_server.session.conversation_utils import ConversationUtils
                from openapi_server.managers.session_manager import SessionManager

                user_messages = [
                    self.session.messages[idx]
                    for idx in self.message_indices
                    if idx < len(self.session.messages) and self.session.messages[idx].get('role') == 'user'
                ]
                if user_messages:
                    event_hash = ConversationUtils.calculate_hash_for_specific_messages(user_messages)
                    session_manager = SessionManager.get_instance()
                    session_manager.unregister_tool_calling_event(event_hash)
                    session_manager.register_timed_out_tool_call(event_hash, self.session.session_id)
            except Exception as map_error:
                logger.error(f"Event {self.event_id}: Failed tool-timeout map cleanup: {map_error}")

            if self._completion_callback:
                await self._completion_callback(self.event_id, self.state)

            return True

    @abstractmethod
    async def execute_turn(self, request_data) -> dict:
        """
        Execute this turn (async).
        Handles all processing including tool calling.
        Returns result when turn is complete.

        Returns:
            {
                "response": content or tool_calls,
                "finish_reason": "stop" or "tool_calls" or "error",
                "needs_tool_response": bool,  # True if waiting for tool response
                "turn_complete": bool  # True when turn is fully complete
            }
        """
        pass

    @abstractmethod
    async def continue_with_tool_response(self, tool_response: str, request_data) -> dict:
        """
        Continue turn after receiving tool response (async).
        Completes the turn.
        """
        pass

    @abstractmethod
    def take_over_handle(self, previous_event: 'ConversationEvent'):
        """Take over handle from previous event."""
        pass

    @abstractmethod
    def create_new_handle(self):
        """Create new handle for this event."""
        pass

    @abstractmethod
    def release_handle(self):
        """Release handle (for next event to take over or cleanup)."""
        pass

    @abstractmethod
    def terminate_handle(self, force: bool = False):
        """
        Forcefully destroy the handle, regardless of ownership.

        Args:
            force: If True, immediately kill the subprocess (SIGKILL).
        """
        pass

    @abstractmethod
    def _calculate_prompt_tokens(self) -> int:
        """Calculate prompt tokens for this turn (implemented by subclasses)."""
        pass

    @abstractmethod
    def _calculate_completion_tokens(self) -> int:
        """Calculate completion tokens for this turn (implemented by subclasses)."""
        pass

    def calculate_token_usage(self):
        """Calculate detailed token breakdown for this turn."""
        # Calculate prompt and completion tokens using subclass implementations
        self.prompt_tokens = self._calculate_prompt_tokens()
        self.completion_tokens = self._calculate_completion_tokens()
        self.total_turn_tokens = self.prompt_tokens + self.completion_tokens

        # Update legacy field for backward compatibility
        self.turn_tokens = self.total_turn_tokens

    def should_cleanup_handle_after_turn(self) -> bool:
        """
        Determine if handle should be cleaned up after turn completion.
        Returns True in ADHOC_MODE, False otherwise.

        In ADHOC_MODE, handles are created and destroyed for each conversation turn
        to prevent QAIRT handle conflicts when multiple containers access the same NSP.
        """
        from openapi_server.impl.constant import ADHOC_MODE
        return ADHOC_MODE

    def complete_turn(self):
        """Mark turn as completed with enhanced token tracking."""
        if self.state == EventState.ACTIVE:
            self.cancel_tool_response_timeout(clear_flags=True)
            self.state = EventState.COMPLETED
            self.completed_at = time.time()

            # Calculate detailed token breakdown
            self.calculate_token_usage()

            # Calculate hash for this turn
            self.calculate_turn_hash()

            # NOTE: Event hash calculation is now done explicitly by the handler
            # after all message indices are added. This ensures the hash includes
            # all messages in the turn (user + assistant).

            logger.info(f"Event {self.event_id}: Turn COMPLETED, "
                       f"prompt: {self.prompt_tokens}, "
                       f"completion: {self.completion_tokens}, "
                       f"total: {self.total_turn_tokens} tokens")

            # ADHOC_MODE: Cleanup handle after turn completion
            # This ensures the handle stays alive during tool calling (when event is ACTIVE)
            # and only gets destroyed when the entire conversation turn is complete
            if self.should_cleanup_handle_after_turn():
                logger.info(f"Event {self.event_id}: ADHOC_MODE enabled - cleaning up handle after turn completion")
                try:
                    self.terminate_handle()
                except Exception as e:
                    logger.error(f"Event {self.event_id}: Error cleaning up handle in ADHOC_MODE: {e}")

            # NOTE: Do NOT trigger the completion callback here.
            # For streaming responses, the callback is called from the generator's
            # finally block AFTER the subprocess sends its final READY signal,
            # guaranteeing the DSP is truly idle before the next request starts.
            # Triggering it here (via create_task) would release the DSP lock
            # prematurely — before the subprocess finishes — causing the next
            # request to receive "A prompt is already being processed".
            # For non-streaming, _process_request_with_lock_held calls the callback
            # directly after detecting event.is_completed().

    def cancel_turn(self):
        """Mark turn as cancelled (due to explicit client cancellation)."""
        if self.state == EventState.ACTIVE:
            self.cancel_tool_response_timeout(clear_flags=True)
            self.state = EventState.CANCELLED
            self.failed_at = time.time()
            logger.info(f"Event {self.event_id}: Turn CANCELLED")

            # Release handle if owned
            if self.handle_owned:
                try:
                    self.release_handle()
                except Exception as e:
                    logger.error(f"Event {self.event_id}: Error releasing handle on cancel: {e}")

            # Trigger completion callback so the DSP lock is always released
            if self._completion_callback:
                asyncio.create_task(self._trigger_completion_callback())

    def fail_turn(self, error: Exception):
        """Mark turn as failed."""
        if self.state == EventState.ACTIVE:
            self.cancel_tool_response_timeout(clear_flags=True)
            self.state = EventState.FAILED
            self.failed_at = time.time()

            self.error_info = {
                "error_type": type(error).__name__,
                "error_message": str(error),
                "retry_count": self.retry_count,
                "failed_at": self.failed_at
            }

            logger.error(f"Event {self.event_id}: Turn FAILED - {error}")

            # Cleanup handle if owned
            if self.handle_owned:
                try:
                    self.release_handle()
                except Exception as e:
                    logger.error(f"Event {self.event_id}: Error releasing handle: {e}")

            # Trigger completion callback so the DSP lock is always released
            if self._completion_callback:
                asyncio.create_task(self._trigger_completion_callback())

    def calculate_turn_hash(self) -> str:
        """Calculate hash for this turn."""
        turn_str = f"user:{self.user_message}|assistant:{self.assistant_message}|"
        self.turn_hash = hashlib.sha256(turn_str.encode()).hexdigest()[:16]
        return self.turn_hash

    def calculate_event_hash(self):
        """
        Calculate hash for this event's messages based on message indices.
        This hash is used for session matching.
        """
        if not self.message_indices:
            logger.debug(f"Event {self.event_id}: No message indices, cannot calculate hash")
            return None

        event_messages = [self.session.messages[idx] for idx in self.message_indices
                         if idx < len(self.session.messages)]

        logger.info(f"Event {self.event_id}: Calculating event hash")
        logger.info(f"  - Message indices: {self.message_indices}")
        logger.info(f"  - Session has {len(self.session.messages)} messages")
        logger.info(f"  - Extracted {len(event_messages)} messages for hash")
        for i, (idx, msg) in enumerate(zip(self.message_indices, event_messages)):
            logger.info(f"  - Message[{idx}]: role={msg.get('role')}, content_preview={str(msg.get('content', ''))[:50]}...")

        from openapi_server.session.conversation_utils import ConversationUtils
        self.event_hash = ConversationUtils.calculate_hash_for_specific_messages(event_messages, debug=True)

        logger.info(f"Event {self.event_id}: Calculated event hash = {self.event_hash}")
        return self.event_hash

    def register_completion_callback(self, callback: Callable):
        """
        Register a callback to be called when this event completes.
        Used by RequestQueueManager in ADHOC_MODE for lock management.

        Args:
            callback: Async function(event_id, event_state) to call on completion
        """
        self._completion_callback = callback
        logger.debug(f"Event {self.event_id}: Registered completion callback")

    async def _trigger_completion_callback(self):
        """
        Trigger the completion callback if registered.
        Called automatically by complete_turn().
        """
        if self._completion_callback:
            try:
                logger.debug(f"Event {self.event_id}: Triggering completion callback (state={self.state.value})")
                await self._completion_callback(self.event_id, self.state)
            except Exception as e:
                logger.error(f"Event {self.event_id}: Error in completion callback: {e}", exc_info=True)

    def is_active(self) -> bool:
        """Check if event is active."""
        return self.state == EventState.ACTIVE

    def is_completed(self) -> bool:
        """Check if event is completed."""
        return self.state == EventState.COMPLETED

    def is_failed(self) -> bool:
        """Check if event has failed."""
        return self.state == EventState.FAILED

    def to_turn(self) -> ConversationTurn:
        """Convert event to ConversationTurn object."""
        turn = ConversationTurn(
            user=self.user_message,
            assistant=self.assistant_message,
            tool=self.tool_info,
            model_used=self.model_id
        )
        turn.tokens = self.turn_tokens
        turn.timestamp = self.created_at
        return turn

    def to_dict(self) -> dict:
        """Convert turn to dictionary."""
        return {
            "event_id": self.event_id,
            "session_id": self.session_id,
            "model_id": self.model_id,
            "event_type": self.event_type.value,
            "state": self.state.value,
            "user_message": self.user_message,
            "assistant_message": self.assistant_message,
            "tool_info": self.tool_info,
            "turn_tokens": self.turn_tokens,
            "turn_hash": self.turn_hash,
            "created_at": self.created_at,
            "completed_at": self.completed_at,
            "failed_at": self.failed_at,
            "error_info": self.error_info,
            "handle_owned": self.handle_owned,
            "handle_borrowed": self.handle_borrowed
        }
