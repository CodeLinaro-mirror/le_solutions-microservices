# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

"""
SessionManager: Singleton to manage all conversation sessions.
Handles session creation, lookup, and cleanup.
"""

import json
import time
import threading
import uuid
from typing import Any, Dict, Optional, List, Tuple

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
    TIMED_OUT_TOOL_CALL_TTL = 300  # 5 minutes

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
            self._timed_out_tool_call_map: Dict[str, Tuple[str, float]] = {}  # event_hash -> (session_id, expires_at)
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
            self._timed_out_tool_call_map.pop(event_hash, None)
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

    def register_timed_out_tool_call(self, event_hash: str, session_id: str, ttl_seconds: Optional[int] = None) -> None:
        """
        Register a timed-out tool calling event hash for deterministic late-response handling.
        """
        expires_at = time.time() + (ttl_seconds if ttl_seconds is not None else self.TIMED_OUT_TOOL_CALL_TTL)
        with self._lock:
            self._tool_calling_map.pop(event_hash, None)
            self._timed_out_tool_call_map[event_hash] = (session_id, expires_at)
            logger.info(f"Registered timed-out tool hash {event_hash[:8]}... for session {session_id}")

    def is_timed_out_tool_call(self, event_hash: str) -> bool:
        """
        Check whether an event hash belongs to a recently timed-out tool call.
        Expired or stale entries are cleaned up automatically.
        """
        with self._lock:
            entry = self._timed_out_tool_call_map.get(event_hash)
            if not entry:
                return False

            session_id, expires_at = entry
            if time.time() > expires_at or session_id not in self._sessions:
                self._timed_out_tool_call_map.pop(event_hash, None)
                return False

            return True

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

    @staticmethod
    def build_request_signature(request_data, raw_json: Optional[dict], messages: List[Dict]) -> str:
        """Build a deterministic signature for an incoming request."""
        if raw_json:
            request_payload = dict(raw_json)
        elif request_data is not None and hasattr(request_data, "model_dump"):
            request_payload = request_data.model_dump(exclude_none=True)
        else:
            request_payload = {}

        request_payload.pop("stream", None)
        request_payload.pop("user", None)
        request_payload.pop("store", None)
        request_payload["messages"] = messages

        return json.dumps(request_payload, sort_keys=True, separators=(",", ":"), default=str)

    def find_or_create_session(
        self,
        user_id: str,
        messages: List[Dict],
        request_data=None,
        raw_json: dict = None
    ) -> Tuple[ConversationSession, bool, Optional[Any]]:
        """
        Find existing session by hash or create new one.

        Args:
            user_id: User identifier
            messages: Message history from request

        Returns:
            Tuple of (ConversationSession, is_new, replay_event)
        """
        from openapi_server.session.conversation_utils import ConversationUtils

        if not messages:
            # No messages - create new session
            chat_completion_id = f"chat-{uuid.uuid4()}"
            session = self.create_session(chat_completion_id, user_id)
            return session, True, None

        # Calculate full completed-pairs hash only.
        # This is used for both continuation and retry-candidate lookup.
        full_hash_key = ConversationUtils.calculate_conversation_hash(messages, exclude_last_pair=False)

        with self._lock:
            # 1. Try tool calling map - for in-progress tool calls
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
                        return session, False, None
                    else:
                        # Stale entry
                        del self._tool_calling_map[tool_call_hash]

                # 2. Try timed-out tool calling map - for late tool responses
                is_tool_response_request = ConversationUtils.safe_get(messages[-1], "role") == "tool"
                if is_tool_response_request and tool_call_hash in self._timed_out_tool_call_map:
                    session_id, expires_at = self._timed_out_tool_call_map[tool_call_hash]
                    if time.time() > expires_at:
                        del self._timed_out_tool_call_map[tool_call_hash]
                    elif session_id in self._sessions:
                        session = self._sessions[session_id]
                        logger.info(f"Found timed-out tool session {session_id} by hash {tool_call_hash[:8]}...")
                        return session, False, None
                    else:
                        del self._timed_out_tool_call_map[tool_call_hash]

            # 3. Try full hash against the current live session tip / retry candidate
            if full_hash_key and full_hash_key in self._hash_to_session:
                session_id = self._hash_to_session[full_hash_key]
                if session_id in self._sessions:
                    session = self._sessions[session_id]
                    if full_hash_key == session.continuation_hash:
                        logger.info(f"Found existing session {session_id} by continuation hash {full_hash_key}")
                        return session, False, None

                    if full_hash_key == session.retry_candidate_hash:
                        replay_event = session.get_last_completed_event()
                        if replay_event and replay_event.request_signature and replay_event.replay_result:
                            incoming_signature = self.build_request_signature(request_data, raw_json, messages)
                            if incoming_signature == replay_event.request_signature:
                                logger.info(f"Found retry replay for session {session_id} by hash {full_hash_key}")
                                return session, False, replay_event

                    if (
                        full_hash_key != session.continuation_hash and
                        full_hash_key != session.retry_candidate_hash
                    ):
                        # Stale hash entry from older behavior
                        del self._hash_to_session[full_hash_key]
                else:
                    del self._hash_to_session[full_hash_key]

            # Create new session
            chat_completion_id = f"chat-{uuid.uuid4()}"
            session = self.create_session(chat_completion_id, user_id)
            logger.info(f"Created new session {chat_completion_id} (hashes will be stored after turn completion)")

            return session, True, None

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

                # Remove from timed-out tool calling map
                timed_out_tool_keys_to_remove = [
                    k for k, (mapped_session_id, _) in self._timed_out_tool_call_map.items()
                    if mapped_session_id == session_id
                ]
                for k in timed_out_tool_keys_to_remove:
                    del self._timed_out_tool_call_map[k]
                logger.debug(
                    f"Removed {len(timed_out_tool_keys_to_remove)} timed-out tool mappings for session {session_id}"
                )

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

                # Remove all hash mappings for this session
                hash_keys_to_remove = [k for k, v in self._hash_to_session.items() if v == session_id]
                for k in hash_keys_to_remove:
                    del self._hash_to_session[k]

                # Remove from tool calling maps
                tool_keys_to_remove = [k for k, v in self._tool_calling_map.items() if v == session_id]
                for k in tool_keys_to_remove:
                    del self._tool_calling_map[k]
                timed_out_tool_keys_to_remove = [
                    k for k, (mapped_session_id, _) in self._timed_out_tool_call_map.items()
                    if mapped_session_id == session_id
                ]
                for k in timed_out_tool_keys_to_remove:
                    del self._timed_out_tool_call_map[k]

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

                    # Remove all hash mappings for this session
                    hash_keys_to_remove = [k for k, v in self._hash_to_session.items() if v == session_id]
                    for k in hash_keys_to_remove:
                        del self._hash_to_session[k]

                    # Remove from tool calling maps
                    tool_keys_to_remove = [k for k, v in self._tool_calling_map.items() if v == session_id]
                    for k in tool_keys_to_remove:
                        del self._tool_calling_map[k]
                    timed_out_tool_keys_to_remove = [
                        k for k, (mapped_session_id, _) in self._timed_out_tool_call_map.items()
                        if mapped_session_id == session_id
                    ]
                    for k in timed_out_tool_keys_to_remove:
                        del self._timed_out_tool_call_map[k]

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
                "hash_mappings": len(self._hash_to_session),
                "tool_call_mappings": len(self._tool_calling_map),
                "timed_out_tool_mappings": len(self._timed_out_tool_call_map)
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
            self._timed_out_tool_call_map.clear()
            logger.warning("Cleared all sessions")
