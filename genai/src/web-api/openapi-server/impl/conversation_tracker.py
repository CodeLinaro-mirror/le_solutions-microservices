# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import threading
import time
from typing import Dict, List, Optional
from dataclasses import dataclass, field
from openapi_server.logger.logger_config import LoggerConfig

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)


@dataclass
class ConversationMessage:
    """Represents a single message in a conversation."""
    role: str
    content: str
    tokens: int
    timestamp: float = field(default_factory=time.time)


@dataclass
class ConversationState:
    """Represents the state of a conversation for a user."""
    messages: List[ConversationMessage] = field(default_factory=list)
    total_tokens: int = 0
    last_summary: Optional[str] = None
    summary_tokens: int = 0
    last_activity: float = field(default_factory=time.time)
    last_system_prompt: Optional[str] = None


class ConversationTracker:
    """
    Singleton class to track conversations per user (safety_identifier).
    Monitors token usage and determines when summarization is needed.
    """
    _instance = None
    _lock = threading.Lock()

    # Maximum number of conversations to track
    MAX_CONVERSATIONS = 100
    # Time to live for inactive conversations (1 hour)
    CONVERSATION_TTL = 3600

    def __new__(cls):
        if cls._instance is None:
            with cls._lock:
                if cls._instance is None:
                    cls._instance = super().__new__(cls)
                    cls._instance._initialized = False
        return cls._instance

    def __init__(self):
        """Initialize the ConversationTracker singleton."""
        if not self._initialized:
            self.conversations: Dict[str, ConversationState] = {}
            self._lock = threading.Lock()
            self._initialized = True
            logger.info("ConversationTracker initialized")

    def add_message(self, safety_id: str, role: str, content: str, token_count: int) -> None:
        """
        Add a message to a user's conversation history.

        Args:
            safety_id: User's safety identifier
            role: Message role (user/assistant)
            content: Message content
            token_count: Estimated token count for the message
        """
        with self._lock:
            if safety_id not in self.conversations:
                self.conversations[safety_id] = ConversationState()
                logger.info(f"Created new conversation for user: {safety_id}")

            conversation = self.conversations[safety_id]
            message = ConversationMessage(
                role=role,
                content=content,
                tokens=token_count
            )
            conversation.messages.append(message)
            conversation.total_tokens += token_count
            conversation.last_activity = time.time()

            logger.debug(f"Added {role} message for {safety_id}: {token_count} tokens, total: {conversation.total_tokens}")

            # Cleanup old conversations if needed
            self._cleanup_old_conversations()

    def get_conversation_history(self, safety_id: str) -> List[ConversationMessage]:
        """
        Get the conversation history for a user.

        Args:
            safety_id: User's safety identifier

        Returns:
            List of conversation messages
        """
        with self._lock:
            if safety_id in self.conversations:
                return self.conversations[safety_id].messages.copy()
            return []

    def get_token_count(self, safety_id: str) -> int:
        """
        Get the total token count for a user's conversation.

        Args:
            safety_id: User's safety identifier

        Returns:
            Total token count
        """
        with self._lock:
            if safety_id in self.conversations:
                return self.conversations[safety_id].total_tokens
            return 0

    def should_summarize(self, safety_id: str, context_size: int, threshold: float = 0.7) -> bool:
        """
        Determine if summarization should be triggered.

        Args:
            safety_id: User's safety identifier
            context_size: Model's context window size
            threshold: Percentage threshold (default 0.7 for 70%)

        Returns:
            True if summarization should be triggered
        """
        token_count = self.get_token_count(safety_id)
        threshold_tokens = int(context_size * threshold)
        should_summarize = token_count >= threshold_tokens

        if should_summarize:
            logger.warning(
                f"Summarization threshold reached for {safety_id}: "
                f"{token_count}/{context_size} tokens ({threshold*100}% threshold)"
            )

        return should_summarize

    def update_with_summary(self, safety_id: str, summary_text: str, summary_tokens: int) -> None:
        """
        Update conversation with a summary, clearing old messages.

        Args:
            safety_id: User's safety identifier
            summary_text: The generated summary
            summary_tokens: Token count of the summary
        """
        with self._lock:
            if safety_id in self.conversations:
                conversation = self.conversations[safety_id]

                # Clear old messages
                old_token_count = conversation.total_tokens
                conversation.messages.clear()

                # Store summary
                conversation.last_summary = summary_text
                conversation.summary_tokens = summary_tokens
                conversation.total_tokens = summary_tokens
                conversation.last_activity = time.time()

                logger.info(
                    f"Updated conversation for {safety_id} with summary: "
                    f"reduced from {old_token_count} to {summary_tokens} tokens"
                )

    def get_last_summary(self, safety_id: str) -> Optional[str]:
        """
        Get the last summary for a user's conversation.

        Args:
            safety_id: User's safety identifier

        Returns:
            Last summary text or None
        """
        with self._lock:
            if safety_id in self.conversations:
                return self.conversations[safety_id].last_summary
            return None

    def update_system_prompt(self, safety_id: str, system_prompt: str) -> None:
        """
        Update the most recent system prompt for a user.

        Args:
            safety_id: User's safety identifier
            system_prompt: The system prompt content to store
        """
        with self._lock:
            if safety_id not in self.conversations:
                self.conversations[safety_id] = ConversationState()
                logger.info(f"Created new conversation for user: {safety_id}")

            self.conversations[safety_id].last_system_prompt = system_prompt
            self.conversations[safety_id].last_activity = time.time()
            logger.debug(f"Updated system prompt for {safety_id}: {len(system_prompt)} chars")

    def get_system_prompt(self, safety_id: str) -> Optional[str]:
        """
        Get the most recent system prompt for a user.

        Args:
            safety_id: User's safety identifier

        Returns:
            Last system prompt text or None
        """
        with self._lock:
            if safety_id in self.conversations:
                return self.conversations[safety_id].last_system_prompt
            return None

    def clear_conversation(self, safety_id: str) -> None:
        """
        Clear a user's conversation history.

        Args:
            safety_id: User's safety identifier
        """
        with self._lock:
            if safety_id in self.conversations:
                del self.conversations[safety_id]
                logger.info(f"Cleared conversation for user: {safety_id}")

    def _cleanup_old_conversations(self) -> None:
        """Remove old inactive conversations to prevent memory bloat."""
        current_time = time.time()
        to_remove = []

        for safety_id, conversation in self.conversations.items():
            if current_time - conversation.last_activity > self.CONVERSATION_TTL:
                to_remove.append(safety_id)

        for safety_id in to_remove:
            del self.conversations[safety_id]
            logger.info(f"Removed inactive conversation for {safety_id}")

        # If still too many, remove oldest
        if len(self.conversations) > self.MAX_CONVERSATIONS:
            sorted_conversations = sorted(
                self.conversations.items(),
                key=lambda x: x[1].last_activity
            )
            to_remove_count = len(self.conversations) - self.MAX_CONVERSATIONS
            for safety_id, _ in sorted_conversations[:to_remove_count]:
                del self.conversations[safety_id]
                logger.info(f"Removed old conversation for {safety_id} (capacity limit)")

    def get_conversation_count(self) -> int:
        """Get the number of active conversations."""
        with self._lock:
            return len(self.conversations)
