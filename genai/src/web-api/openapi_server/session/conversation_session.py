# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
ConversationSession: Manages a conversation session with shared message history.
Each session has a stable chat completion ID and stores messages in OpenAI format.
Events reference indices into the shared message history.
"""

import json
import hashlib
from typing import List, Optional, Dict
from datetime import datetime

from openapi_server.events.conversation_event import ConversationEvent, EventType
from openapi_server.events.text_conversation_event import TextConversationEvent
from openapi_server.events.vision_conversation_event import VisionConversationEvent
from openapi_server.managers.model_config_manager import ModelConfigManager
from openapi_server.logger.logger_config import LoggerConfig

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)


class ConversationSession:
    """
    Manages a conversation session with shared message history.

    Key concepts:
    - session_id = chat completion ID (stable across conversation)
    - messages = shared OpenAI-format message history (List[Dict])
    - events = list of ConversationEvents that reference message indices
    - Each event tracks which messages it's responsible for via message_indices
    """

    def __init__(self, chat_completion_id: str, user_id: str):
        """
        Initialize a new conversation session.

        Args:
            chat_completion_id: The chat completion ID (e.g., "chat-abc123")
            user_id: User identifier
        """
        self.session_id = chat_completion_id  # Stable ID for this conversation
        self.user_id = user_id

        # Shared conversation history in OpenAI format
        self.messages: List[Dict] = []

        # List of events (each event references message indices)
        self.events: List[ConversationEvent] = []

        # Current active event (if any)
        self.current_event: Optional[ConversationEvent] = None

        # Current model being used
        self.current_model: Optional[str] = None

        # Session metadata
        self.created_at = datetime.now()
        self.last_activity = datetime.now()

        # Cumulative token tracking
        self.total_cumulative_tokens = 0

        # Summarization tracking
        self.summary_content = ""
        self.summary_token_count = 0

        logger.info(f"Created session {chat_completion_id} for user {user_id}")

    def add_message(self, message: Dict) -> int:
        """
        Add a message to the shared history.

        Args:
            message: OpenAI-format message dict

        Returns:
            Index of the added message
        """
        self.messages.append(message)
        self.last_activity = datetime.now()
        idx = len(self.messages) - 1

        logger.debug(f"Session {self.session_id}: Added message at index {idx}: {message.get('role')}")
        return idx

    def get_message(self, index: int) -> Optional[Dict]:
        """Get message at specific index."""
        if 0 <= index < len(self.messages):
            return self.messages[index]
        return None

    def get_messages_slice(self, start: int, end: Optional[int] = None) -> List[Dict]:
        """Get slice of messages."""
        if end is None:
            return self.messages[start:]
        return self.messages[start:end]

    def calculate_hash(self, exclude_last_pair: bool = False) -> str:
        """
        Calculate hash from messages using ConversationUtils logic.

        Args:
            exclude_last_pair: If True, exclude last complete pair (for lookup).
                             If False, include all (for storage).

        Returns:
            16-char hash
        """
        from openapi_server.session.conversation_utils import ConversationUtils
        return ConversationUtils.calculate_conversation_hash(self.messages, exclude_last_pair)

    def create_event(
        self,
        model_id: str,
        new_messages: List[Dict],
        is_tool_continuation: bool = False
    ) -> ConversationEvent:
        """
        Create a new event for processing new messages.

        Args:
            model_id: Model to use for this event
            new_messages: New messages to add to history
            is_tool_continuation: Whether this continues a tool calling event

        Returns:
            New ConversationEvent
        """
        # Determine event type from model config
        config_manager = ModelConfigManager()
        is_vision_model = config_manager.supports_vision(model_id)

        # Create appropriate event type
        if is_vision_model:
            event = VisionConversationEvent(
                session=self,
                model_id=model_id
            )
        else:
            event = TextConversationEvent(
                session=self,
                model_id=model_id
            )

        # Store the number of new messages before adding them
        num_new_messages = len(new_messages)

        # Add new messages to history and track indices
        for msg in new_messages:
            idx = self.add_message(msg)
            event.message_indices.append(idx)

        # Handle management
        if not is_tool_continuation:
            previous_event = self.get_last_completed_event()
            model_switched = previous_event and previous_event.model_id != model_id

            if model_switched:
                logger.info(f"Session {self.session_id}: Model switch detected from {previous_event.model_id} to {model_id}")

                # Prepare messages for summarization (exclude the new messages just added)
                messages_for_summary = self.messages[:-num_new_messages] if num_new_messages > 0 else self.messages
                logger.info(f"Summarizing {len(messages_for_summary)} historical messages (excluding {num_new_messages} new messages)")

                # Case A: Text-to-Text switch (summarize with old handle)
                if isinstance(previous_event, TextConversationEvent) and isinstance(event, TextConversationEvent):
                    logger.info("Performing Text-to-Text switch summarization...")
                    try:
                        max_summary_tokens = int(previous_event.context_size * 0.2) # Allow larger summary for switches
                        summary_text, _ = previous_event.generate_summary(max_summary_tokens, messages_to_summarize=messages_for_summary, include_history_in_prompt=False)
                        self.summary_content = summary_text
                        event.inject_summary = True
                        logger.info("Successfully generated summary with old handle.")
                    except Exception as e:
                        logger.error(f"Failed to generate summary during model switch: {e}")
                    finally:
                        previous_event.terminate_handle()

                # Case B: Vision-to-Text switch (summarize with new handle)
                elif isinstance(previous_event, VisionConversationEvent) and isinstance(event, TextConversationEvent):
                    logger.info("Performing Vision-to-Text switch summarization...")
                    # First, create the new handle for the text event
                    event.create_new_handle()

                    try:
                        # Use the historical messages (excluding new ones) for summarization
                        # This ensures we don't include the current question in the summary

                        # Check token count against 50% of context size
                        from openapi_server.session.token_counter import TokenCounter
                        token_count = sum(TokenCounter.estimate_tokens(m.get('content', '')) for m in messages_for_summary)

                        if token_count < (event.context_size * 0.5):
                            max_summary_tokens = int(event.context_size * 0.1)
                            summary_text, _ = event.generate_summary(max_summary_tokens, messages_to_summarize=messages_for_summary)
                            self.summary_content = summary_text
                            event.inject_summary = True
                            # Reset handle to clear summarization context before main inference
                            event._reset_handle()
                            logger.info("Successfully generated summary with new handle.")
                        else:
                            logger.warning("Token count of recent history exceeds threshold, skipping summarization.")
                    except Exception as e:
                        logger.error(f"Failed to generate summary for Vision-to-Text switch: {e}")

                    # Terminate vision (no-op but good practice)
                    previous_event.terminate_handle()

                # Case C: Any-to-Vision or other switches
                else:
                    logger.info("Switching models. Terminating old handle and creating new one without summarization.")
                    previous_event.terminate_handle()

                # Create a new handle if not already created (e.g., in Text-to-Text)
                if not event.llm_handle:
                    event.create_new_handle()

            elif previous_event: # Same model
                try:
                    event.take_over_handle(previous_event)
                except Exception as e:
                    logger.warning(f"Failed to take over handle: {e}. Creating new handle.")
                    event.create_new_handle()
            else: # No previous event
                event.create_new_handle()

        # Set as current event
        self.current_event = event
        self.current_model = model_id
        self.last_activity = datetime.now()

        logger.info(f"Session {self.session_id}: Created event {event.event_id} for model {model_id}")

        return event

    def complete_current_event(self):
        """Complete current event and add to history."""
        if self.current_event and self.current_event.is_completed():
            # Update cumulative token tracking
            self.total_cumulative_tokens += self.current_event.total_turn_tokens

            # Update event's cumulative token tracking
            self.current_event.cumulative_tokens_before = self.total_cumulative_tokens - self.current_event.total_turn_tokens
            self.current_event.cumulative_tokens_after = self.total_cumulative_tokens

            # Add to events list
            self.events.append(self.current_event)

            # UPDATE SESSION HASH MAPPING (n-1)
            # Now that turn is complete and in history, update the hash mapping so
            # future requests (which will include this turn) can find this session.
            try:
                full_hash = self.calculate_hash(exclude_last_pair=False)
                if full_hash:
                    from openapi_server.managers.session_manager import SessionManager
                    session_manager = SessionManager.get_instance()
                    with session_manager._lock:
                        session_manager._hash_to_session[full_hash] = self.session_id
                    logger.info(f"Session {self.session_id}: Updated hash mapping for {full_hash}")
            except Exception as e:
                logger.error(f"Session {self.session_id}: Failed to update hash mapping: {e}")

            logger.info(f"Session {self.session_id}: Completed event {self.current_event.event_id}, "
                       f"Turn tokens: {self.current_event.total_turn_tokens}, "
                       f"Cumulative: {self.total_cumulative_tokens}")

            # Don't clear current_event yet - next event may need to take over handle

    def get_current_event(self) -> Optional[ConversationEvent]:
        """Get current active event."""
        if self.current_event and self.current_event.is_active():
            return self.current_event
        return None

    def get_last_completed_event(self) -> Optional[ConversationEvent]:
        """Get last completed event (for handle takeover)."""
        # Check if current event is completed
        if self.current_event and self.current_event.is_completed():
            return self.current_event

        # Otherwise get last from history
        if self.events:
            return self.events[-1]

        return None

    def get_event_messages(self, event: ConversationEvent) -> List[Dict]:
        """Get all messages associated with an event."""
        return [self.messages[idx] for idx in event.message_indices if idx < len(self.messages)]

    def to_dict(self) -> dict:
        """Convert session to dictionary for debugging/serialization."""
        return {
            "session_id": self.session_id,
            "user_id": self.user_id,
            "message_count": len(self.messages),
            "messages": self.messages,
            "event_count": len(self.events),
            "current_event_id": self.current_event.event_id if self.current_event else None,
            "current_model": self.current_model,
            "created_at": self.created_at.isoformat(),
            "last_activity": self.last_activity.isoformat()
        }

    def get_conversation_summary(self) -> str:
        """Get a summary of the conversation for logging."""
        return (
            f"Session {self.session_id}: "
            f"{len(self.messages)} messages, "
            f"{len(self.events)} completed events, "
            f"model={self.current_model}"
        )

    def get_last_summarization_event(self) -> Optional[ConversationEvent]:
        """Find the most recent event where summarization occurred."""
        for event in reversed(self.events):
            if hasattr(event, 'summarization_performed') and event.summarization_performed:
                return event
        return None

    def calculate_tokens_since_last_summarization(self) -> int:
        """Calculate tokens accumulated since last summarization."""
        last_summary_event = self.get_last_summarization_event()

        if last_summary_event:
            # Tokens since last summarization = current_total - tokens_at_last_summarization_event
            tokens_since = self.total_cumulative_tokens - last_summary_event.cumulative_tokens_after

            logger.debug(f"Tokens since last summarization: {self.total_cumulative_tokens} - "
                        f"{last_summary_event.cumulative_tokens_after} = {tokens_since}")

            return tokens_since
        else:
            # No summarization yet - count all tokens
            return self.total_cumulative_tokens

    def should_trigger_summarization(self, projected_new_tokens: int, context_size: int) -> bool:
        """Check if summarization should be triggered."""
        # Skip for vision models
        if self.current_model and ModelConfigManager().supports_vision(self.current_model):
            return False

        # Need events to summarize
        if len(self.events) < 2:
            return False

        tokens_since_last_summary = self.calculate_tokens_since_last_summarization()
        projected_total = tokens_since_last_summary + projected_new_tokens
        threshold = context_size * 0.7

        should_summarize = projected_total >= threshold

        if should_summarize:
            logger.info(f"Summarization threshold reached: "
                       f"tokens_since_last_summary={tokens_since_last_summary}, "
                       f"projected_new={projected_new_tokens}, "
                       f"total_projected={projected_total}, "
                       f"threshold={threshold}")

        return should_summarize

    # Removed unused summarization methods as they are now handled directly in TextConversationEvent
