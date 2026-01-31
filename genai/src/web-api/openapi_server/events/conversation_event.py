# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
ConversationEvent: Represents a single complete conversation turn.
Each event handles one user message → assistant response cycle.
"""

import time
import hashlib
from enum import Enum
from abc import ABC, abstractmethod
from typing import Optional, Any, List, Dict
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

        # Internal state (tool calling)
        self._is_tool_calling = False
        self._pending_tool_calls: List[dict] = []
        self._tool_response_received = False

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
    def terminate_handle(self):
        """Forcefully destroy the handle, regardless of ownership."""
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

    def fail_turn(self, error: Exception):
        """Mark turn as failed."""
        if self.state == EventState.ACTIVE:
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
