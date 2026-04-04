# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
SessionManager: Singleton to manage all conversation sessions.
Handles session creation, lookup, and cleanup.
"""

import time
import threading
import uuid
from typing import Dict, Optional, List, Tuple

from openapi_server.session.conversation_session import ConversationSession
from openapi_server.logger.logger_config import LoggerConfig

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)


class SessionManager:
    """
    Singleton to manage all conversation sessions.
    Handles session creation, lookup by hash, and cleanup.
    """

    _instance = None
    _lock = threading.Lock()

    MAX_SESSIONS = 100
    SESSION_TTL = 3600  # 1 hour

    def __new__(cls):
        if cls._instance is None:
            with cls._lock:
                if cls._instance is None:
                    cls._instance = super().__new__(cls)
                    cls._instance._initialized = False
        return cls._instance

    def __init__(self):
        """Initialize the SessionManager singleton."""
        if not self._initialized:
            self._sessions: Dict[str, ConversationSession] = {}  # session_id -> session
            self._hash_to_session: Dict[str, str] = {}  # hash -> session_id (for completed turns)
            self._tool_calling_map: Dict[str, str] = {}  # event_hash -> session_id (for in-progress tool calls)
            self._lock = threading.RLock()
            self._initialized = True
            logger.info("SessionManager initialized")

    @classmethod
    def get_instance(cls) -> 'SessionManager':
        """Get the singleton instance."""
        return cls()

    def register_tool_calling_event(self, event_hash: str, session_id: str) -> None:
        """
        Register a tool calling event hash to session mapping.
        This allows fallback lookup for in-progress tool calling events.

        Args:
            event_hash: Hash of the messages that initiated tool calling
            session_id: Session identifier
        """
        with self._lock:
            self._tool_calling_map[event_hash] = session_id
            logger.info(f"Registered tool calling event hash {event_hash[:8]}... for session {session_id}")

    def unregister_tool_calling_event(self, event_hash: str) -> None:
        """
        Unregister a tool calling event hash when tool calling completes.

        Args:
            event_hash: Hash of the messages that initiated tool calling
        """
        with self._lock:
            if event_hash in self._tool_calling_map:
                session_id = self._tool_calling_map[event_hash]
                del self._tool_calling_map[event_hash]
                logger.info(f"Unregistered tool calling event hash {event_hash[:8]}... for session {session_id}")

    def create_session(self, chat_completion_id: str, user_id: str) -> ConversationSession:
        """
        Create a new conversation session with a specific chat completion ID.

        Args:
            chat_completion_id: The chat completion ID (e.g., "chat-abc123")
            user_id: User identifier

        Returns:
            New ConversationSession
        """
        session = ConversationSession(chat_completion_id, user_id)

        with self._lock:
            self._sessions[chat_completion_id] = session
            self._cleanup_old_sessions()

        logger.info(f"Created session {chat_completion_id} for user {user_id}")
        return session

    def find_or_create_session(
        self,
        user_id: str,
        messages: List[Dict]
    ) -> Tuple[ConversationSession, bool]:
        """
        Find existing session by hash or create new one.

        Args:
            user_id: User identifier
            messages: Message history from request

        Returns:
            Tuple of (ConversationSession, is_new)
        """
        from openapi_server.session.conversation_utils import ConversationUtils

        if not messages:
            # No messages - create new session
            chat_completion_id = f"chat-{uuid.uuid4()}"
            session = self.create_session(chat_completion_id, user_id)
            return session, True

        # Calculate hash using n-2 logic (exclude last pair) to find previous state
        hash_key = ConversationUtils.calculate_conversation_hash(messages, exclude_last_pair=True)

        # Also calculate n-1 hash (include last pair) for storing new session state
        # This handles case where client sends only 1 message (n-2 hash is empty)
        full_hash_key = ConversationUtils.calculate_conversation_hash(messages, exclude_last_pair=False)

        with self._lock:
            def should_reuse_session(candidate_session: ConversationSession, match_type: str, key: str) -> bool:
                incoming_count = len(messages)
                existing_count = len(candidate_session.messages)

                if incoming_count < existing_count:
                    current_event = candidate_session.get_current_event()
                    is_active_tool_turn = (
                        current_event and
                        current_event.is_active() and
                        getattr(current_event, '_is_tool_calling', False)
                    )
                    if not is_active_tool_turn:
                        logger.debug(
                            f"Skipping {match_type} match {key}: session {candidate_session.session_id} has newer history "
                            f"(incoming_messages={incoming_count}, session_messages={existing_count})"
                        )
                        return False

                return True

            # 1. Try to find by hash (n-2) - standard lookup for continuing conversations
            if hash_key and hash_key in self._hash_to_session:
                session_id = self._hash_to_session[hash_key]
                if session_id in self._sessions:
                    session = self._sessions[session_id]
                    if should_reuse_session(session, "hash", hash_key):
                        logger.info(f"Found existing session {session_id} by hash {hash_key}")
                        return session, False

            # 2. Try full hash (n-1) - for retries or identical requests
            if full_hash_key and full_hash_key in self._hash_to_session:
                session_id = self._hash_to_session[full_hash_key]
                if session_id in self._sessions:
                    session = self._sessions[session_id]
                    if should_reuse_session(session, "full hash", full_hash_key):
                        logger.info(f"Found existing session {session_id} by full hash {full_hash_key}")
                        return session, False

            # 3. Try tool calling map - for in-progress tool calls
            # Calculate hash of ONLY user messages to match the registration logic
            # This ensures stability even if assistant messages vary slightly or are missing in client request
            user_messages = [msg for msg in messages if msg.get('role') == 'user']
            if user_messages:
                tool_call_hash = ConversationUtils.calculate_hash_for_specific_messages(user_messages)
                if tool_call_hash in self._tool_calling_map:
                    session_id = self._tool_calling_map[tool_call_hash]
                    if session_id in self._sessions:
                        session = self._sessions[session_id]
                        logger.info(f"Found existing session {session_id} by tool calling hash {tool_call_hash[:8]}...")
                        return session, False
                    else:
                        # Stale entry
                        del self._tool_calling_map[tool_call_hash]

            # Create new session
            chat_completion_id = f"chat-{uuid.uuid4()}"
            session = self.create_session(chat_completion_id, user_id)

            # Store hash mapping using the n-2 hash (so next request can find it)
            # Wait, if we create a new session with [Msg1], n-2 is empty.
            # Next request comes with [Msg1, Msg2]. n-2 hash of that is hash([Msg1]).
            # So we should store the hash of the CURRENT state of the session.
            # But find_or_create_session doesn't know the future state.
            # It only knows the current messages.
            # If this is a NEW session, it likely has 1 user message. n-2 hash is empty.
            # We don't store empty hash.
            # We should store the hash AFTER the turn completes.

            # However, if the request has history that we don't know about (e.g. client restart),
            # we might want to store the full hash so exact retries work.
            if full_hash_key:
                self._hash_to_session[full_hash_key] = chat_completion_id
                logger.info(f"Created new session {chat_completion_id} with full hash {full_hash_key}")
            else:
                logger.info(f"Created new session {chat_completion_id} (no hash stored yet)")

            return session, True

    def get_session(self, session_id: str) -> Optional[ConversationSession]:
        """
        Get session by ID.

        Args:
            session_id: Session identifier (chat completion ID)

        Returns:
            ConversationSession or None if not found
        """
        with self._lock:
            return self._sessions.get(session_id)

    def get_user_sessions(self, user_id: str) -> List[ConversationSession]:
        """
        Get all sessions for a specific user.

        Args:
            user_id: User identifier

        Returns:
            List of ConversationSession objects
        """
        with self._lock:
            user_sessions = [s for s in self._sessions.values() if s.user_id == user_id]
            logger.debug(f"Found {len(user_sessions)} sessions for user {user_id}")
            return user_sessions

    def delete_session(self, session_id: str) -> bool:
        """
        Delete a session and cleanup all associated resources.

        This includes:
        - Terminating handle from the most recent event only (to prevent double-free)
        - Resetting LLM singleton in ADHOC_MODE
        - Removing hash mappings
        - Removing tool calling mappings

        Args:
            session_id: Session identifier (chat completion ID)

        Returns:
            True if deleted, False if not found
        """
        with self._lock:
            if session_id in self._sessions:
                session = self._sessions[session_id]

                # IMPORTANT: Only cleanup handle from the most recent event
                # In ADHOC_MODE=false, handles are borrowed forward through the event chain,
                # so all events share the same handle pointer. Destroying it multiple times
                # causes "double free or corruption" errors.
                # We only need to destroy the handle once from the latest event.

                # Try current_event first (most common case - active session)
                if session.current_event and session.current_event.llm_handle:
                    try:
                        logger.info(f"Terminating handle from current event {session.current_event.event_id}")
                        session.current_event.terminate_handle()
                    except Exception as e:
                        logger.error(f"Error terminating current event handle: {e}")

                # If no current_event, try the last completed event
                elif session.events:
                    last_event = session.events[-1]
                    if last_event.llm_handle:
                        try:
                            logger.info(f"Terminating handle from last event {last_event.event_id}")
                            last_event.terminate_handle()
                        except Exception as e:
                            logger.error(f"Error terminating last event handle: {e}")

                # In ADHOC_MODE, reset the LLM singleton
                from openapi_server.impl.constant import ADHOC_MODE
                if ADHOC_MODE:
                    try:
                        from openapi_server.impl.genie_wrapper.gen_ai_service_singleton import LLMService
                        logger.info(f"ADHOC_MODE: Resetting LLMService singleton during session deletion")
                        LLMService.reset_singleton()
                    except Exception as e:
                        logger.error(f"Error resetting LLM singleton: {e}")

                # Remove from hash mapping
                # We iterate to remove all entries pointing to this session
                keys_to_remove = [k for k, v in self._hash_to_session.items() if v == session_id]
                for k in keys_to_remove:
                    del self._hash_to_session[k]
                logger.debug(f"Removed {len(keys_to_remove)} hash mappings for session {session_id}")

                # Remove from tool calling map
                tool_keys_to_remove = [k for k, v in self._tool_calling_map.items() if v == session_id]
                for k in tool_keys_to_remove:
                    del self._tool_calling_map[k]
                logger.debug(f"Removed {len(tool_keys_to_remove)} tool calling mappings for session {session_id}")

                # Delete the session
                del self._sessions[session_id]
                logger.info(f"Successfully deleted session {session_id} with complete resource cleanup")
                return True

        logger.warning(f"Session {session_id} not found for deletion")
        return False

    def _cleanup_old_sessions(self):
        """
        Remove old inactive sessions to prevent memory bloat.
        Called automatically when creating new sessions.
        """
        from datetime import datetime, timedelta

        current_time = datetime.now()
        to_remove = []

        with self._lock:
            # Find sessions that have exceeded TTL
            for session_id, session in self._sessions.items():
                time_diff = (current_time - session.last_activity).total_seconds()
                if time_diff > self.SESSION_TTL:
                    to_remove.append(session_id)

            # Remove old sessions
            for session_id in to_remove:
                session = self._sessions[session_id]

                # Cleanup current event's handle
                if session.current_event:
                    try:
                        session.current_event.release_handle()
                    except Exception as e:
                        logger.error(f"Error releasing handle during cleanup: {e}")

                # Remove from hash mapping
                hash_key = session.calculate_hash()
                if hash_key in self._hash_to_session:
                    del self._hash_to_session[hash_key]

                del self._sessions[session_id]
                logger.info(f"Removed inactive session {session_id} (TTL exceeded)")

            # If still too many sessions, remove oldest
            if len(self._sessions) > self.MAX_SESSIONS:
                sorted_sessions = sorted(
                    self._sessions.items(),
                    key=lambda x: x[1].last_activity
                )
                to_remove_count = len(self._sessions) - self.MAX_SESSIONS

                for session_id, session in sorted_sessions[:to_remove_count]:
                    # Cleanup handle
                    if session.current_event:
                        try:
                            session.current_event.release_handle()
                        except Exception as e:
                            logger.error(f"Error releasing handle during capacity cleanup: {e}")

                    # Remove from hash mapping
                    hash_key = session.calculate_hash()
                    if hash_key in self._hash_to_session:
                        del self._hash_to_session[hash_key]

                    del self._sessions[session_id]
                    logger.info(f"Removed old session {session_id} (capacity limit)")

    def get_session_count(self) -> int:
        """
        Get the number of active sessions.

        Returns:
            Number of sessions
        """
        with self._lock:
            return len(self._sessions)

    def get_stats(self) -> Dict:
        """
        Get statistics about sessions.

        Returns:
            Dictionary with session statistics
        """
        with self._lock:
            total_sessions = len(self._sessions)
            total_events = sum(len(s.events) for s in self._sessions.values())
            active_events = sum(1 for s in self._sessions.values() if s.current_event and s.current_event.is_active())

            users = set(s.user_id for s in self._sessions.values())

            return {
                "total_sessions": total_sessions,
                "total_events": total_events,
                "active_events": active_events,
                "unique_users": len(users),
                "max_sessions": self.MAX_SESSIONS,
                "session_ttl": self.SESSION_TTL,
                "hash_mappings": len(self._hash_to_session)
            }

    def clear_all_sessions(self):
        """
        Clear all sessions (for testing/debugging).
        WARNING: This will cleanup all handles and remove all sessions.
        """
        with self._lock:
            for session in self._sessions.values():
                if session.current_event:
                    try:
                        session.current_event.release_handle()
                    except Exception as e:
                        logger.error(f"Error releasing handle during clear: {e}")

            self._sessions.clear()
            self._hash_to_session.clear()
            self._tool_calling_map.clear()
            logger.warning("Cleared all sessions")
