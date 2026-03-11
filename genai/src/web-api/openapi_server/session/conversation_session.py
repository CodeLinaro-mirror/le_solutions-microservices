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
    BASE64_IMAGE_PREFIX_LENGTH = 256

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

    def add_message(self, message: Dict, compact_images: bool = True) -> int:
        """
        Add a message to the shared history.

        Args:
            message: OpenAI-format message dict
            compact_images: Whether to compact older base64 images after appending

        Returns:
            Index of the added message
        """
        self.messages.append(message)
        if compact_images and self._message_contains_base64_image(message):
            self._compact_historical_base64_images()

        self.last_activity = datetime.now()
        idx = len(self.messages) - 1

        logger.debug(f"Session {self.session_id}: Added message at index {idx}: {message.get('role')}")
        return idx

    @staticmethod
    def _safe_get(obj, key, default=None):
        if isinstance(obj, dict):
            return obj.get(key, default)
        return getattr(obj, key, default)

    @classmethod
    def _extract_image_url_value(cls, image_item) -> str:
        image_url_data = cls._safe_get(image_item, 'image_url', {})
        if isinstance(image_url_data, dict):
            return image_url_data.get('url', '') or ''
        if isinstance(image_url_data, str):
            return image_url_data

        url_value = getattr(image_url_data, 'url', '')
        return url_value if isinstance(url_value, str) else ''

    @staticmethod
    def _is_base64_data_url(url: str) -> bool:
        lowered = url.lower()
        return lowered.startswith('data:image') and ';base64,' in lowered

    @classmethod
    def _is_raw_base64_image(cls, url: str) -> bool:
        candidate = url.strip()
        if len(candidate) <= cls.BASE64_IMAGE_PREFIX_LENGTH:
            return False
        if candidate.startswith(('http://', 'https://', 'data:')):
            return False

        allowed_chars = set('ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=_-\n\r')
        return all(char in allowed_chars for char in candidate)

    @classmethod
    def _is_base64_image_url(cls, url: str) -> bool:
        return cls._is_base64_data_url(url) or cls._is_raw_base64_image(url)

    @classmethod
    def _iter_image_items(cls, message):
        content = cls._safe_get(message, 'content')
        if not isinstance(content, list):
            return

        for item_index, item in enumerate(content):
            if cls._safe_get(item, 'type') != 'image_url':
                continue

            url = cls._extract_image_url_value(item)
            if isinstance(url, str) and url:
                yield item_index, item, url

    @classmethod
    def _message_contains_base64_image(cls, message: Dict) -> bool:
        for _, _, url in cls._iter_image_items(message):
            if cls._is_base64_image_url(url):
                return True
        return False

    @classmethod
    def _build_compacted_base64_url(cls, image_url: str) -> str:
        if cls._is_base64_data_url(image_url):
            prefix, payload = image_url.split(',', 1)
            if len(payload) <= cls.BASE64_IMAGE_PREFIX_LENGTH:
                return image_url
            return f"{prefix},{payload[:cls.BASE64_IMAGE_PREFIX_LENGTH]}"

        if len(image_url) <= cls.BASE64_IMAGE_PREFIX_LENGTH:
            return image_url

        return image_url[:cls.BASE64_IMAGE_PREFIX_LENGTH]

    @staticmethod
    def _set_image_url_value(image_item, new_url: str) -> bool:
        if isinstance(image_item, dict):
            image_url_data = image_item.get('image_url')
            if isinstance(image_url_data, dict):
                image_url_data['url'] = new_url
            else:
                image_item['image_url'] = {'url': new_url}
            return True

        image_url_data = getattr(image_item, 'image_url', None)
        if isinstance(image_url_data, dict):
            image_url_data['url'] = new_url
            return True

        if hasattr(image_url_data, 'url'):
            setattr(image_url_data, 'url', new_url)
            return True

        try:
            setattr(image_item, 'image_url', {'url': new_url})
            return True
        except Exception:
            return False

    def _compact_historical_base64_images(self):
        """
        Keep the latest base64 image intact and compact older base64 images
        to the first BASE64_IMAGE_PREFIX_LENGTH characters.
        """
        base64_image_items = []
        for message_index, message in enumerate(self.messages):
            for item_index, item, url in self._iter_image_items(message):
                if self._is_base64_image_url(url):
                    base64_image_items.append((message_index, item_index, item, url))

        if len(base64_image_items) <= 1:
            return

        latest_message_index, latest_item_index, _, _ = base64_image_items[-1]
        compacted_count = 0

        for message_index, item_index, item, url in base64_image_items[:-1]:
            if message_index == latest_message_index and item_index == latest_item_index:
                continue

            compacted_url = self._build_compacted_base64_url(url)
            if compacted_url == url:
                continue

            if self._set_image_url_value(item, compacted_url):
                compacted_count += 1

        if compacted_count:
            logger.info(
                f"Session {self.session_id}: Compacted {compacted_count} historical base64 image(s) to "
                f"{self.BASE64_IMAGE_PREFIX_LENGTH} chars"
            )

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
            idx = self.add_message(msg, compact_images=False)
            event.message_indices.append(idx)

        # Keep only the latest base64 image unmodified in stored history.
        if any(self._message_contains_base64_image(msg) for msg in new_messages):
            self._compact_historical_base64_images()

        # Handle management
        if not is_tool_continuation:
            from openapi_server.impl.constant import ADHOC_MODE

            previous_event = self.get_last_completed_event()
            model_switched = previous_event and previous_event.model_id != model_id

            if ADHOC_MODE:
                # ADHOC_MODE: Always request a new handle (which will use the cache)
                logger.info(f"Session {self.session_id}: ADHOC_MODE enabled - requesting handle from cache")

                # Handle VLM model switching: Only terminate VLM handle when switching to a different VLM model
                # Keep VLM handle alive when switching to LLM (user requirement)
                if (previous_event and
                    isinstance(previous_event, VisionConversationEvent) and
                    isinstance(event, VisionConversationEvent) and
                    previous_event.model_id != model_id):
                    logger.info(f"Session {self.session_id}: VLM model switch detected ({previous_event.model_id} -> {model_id}), terminating old VLM handle")
                    previous_event.terminate_handle()

                # For LLM: LLMService.get_or_create_handle automatically manages single-model constraint
                # For VLM: VLMWrapper caches handles automatically during execution via _get_or_create_handle()
                # Only call create_new_handle for LLM events (VLM's create_new_handle is a no-op)
                if isinstance(event, TextConversationEvent):
                    event.create_new_handle()

            elif model_switched:
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

            elif previous_event: # Same model, not ADHOC_MODE
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

    def cancel_active_event(self) -> bool:
        """
        Cancel the currently active event and roll back session state.

        This method:
        1. Marks the event as cancelled (suppresses error responses to client)
        2. Force-kills the subprocess immediately (SIGKILL)
        3. Removes messages added by the cancelled event from history
        4. Clears the current event reference

        Returns:
            bool: True if an active event was cancelled, False if no active event
        """
        if not self.current_event:
            logger.warning(f"Session {self.session_id}: No current event to cancel")
            return False

        if not self.current_event.is_active():
            logger.warning(f"Session {self.session_id}: Current event {self.current_event.event_id} is not active (state={self.current_event.state})")
            return False

        event = self.current_event
        logger.info(f"Session {self.session_id}: Cancelling active event {event.event_id}")

        # Step 1: Mark event as cancelled so stream error handlers suppress error payloads
        event.is_cancelled = True
        logger.info(f"Session {self.session_id}: Marked event {event.event_id} as cancelled")

        # Step 2: Force-kill the subprocess immediately (SIGKILL)
        try:
            event.terminate_handle(force=True)
            logger.info(f"Session {self.session_id}: Force terminated handle for event {event.event_id}")
        except Exception as e:
            logger.error(f"Session {self.session_id}: Error terminating handle: {e}", exc_info=True)

        # Step 3: Roll back messages added by this event
        if event.message_indices:
            indices_to_remove = sorted(event.message_indices, reverse=True)
            for idx in indices_to_remove:
                if idx < len(self.messages):
                    removed_msg = self.messages.pop(idx)
                    logger.debug(f"Session {self.session_id}: Removed message at index {idx}: {removed_msg.get('role')}")
                else:
                    logger.warning(f"Session {self.session_id}: Index {idx} out of range (len={len(self.messages)})")
            logger.info(f"Session {self.session_id}: Rolled back {len(indices_to_remove)} messages")

        # Step 4: Clear current event
        self.current_event = None
        self.last_activity = datetime.now()

        logger.info(f"Session {self.session_id}: Successfully cancelled event {event.event_id}")
        return True

    # Removed unused summarization methods as they are now handled directly in TextConversationEvent
