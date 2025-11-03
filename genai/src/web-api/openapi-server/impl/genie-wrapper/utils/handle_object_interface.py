# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import threading
from typing import Any
from openapi_server.logger.logger_config import LoggerConfig
from openapi_server.impl.genie_wrapper.gen_ai_service_singleton import LLMService

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)

class HandleObject:
    """
    A class represents object to be held by the map represented by class HandleIdObjectMap.
    Each HandleObject represents a conversation thread for a specific user.
    """
    handle_object = None
    messages_count = 0
    model_id = None
    safety_identifier = None
    total_conversation_tokens = 0
    message_pairs_count = 0
    conversation_hash = ""
    thread_number = 0
    completion_id = ""
    last_summarization_index = -1
    summary_token_count = 0

    def __init__(self, handle_obj:Any, msg_count: int = 1, model_id: str = None,
                 safety_identifier: str = None, thread_number: int = 0):
        """
        Initializes an instance of HandleObject.

        Args:
            handle_obj: Can be of any data type (LLM handle pointer).
            msg_count (int): Initial message count.
            model_id (str): The model identifier for this handle.
            safety_identifier (str): User/client identifier for session tracking.
            thread_number (int): Thread number for this user (0, 1, 2, ...).
        """
        self.handle_object = handle_obj
        self.messages_count = msg_count
        self.model_id = model_id
        self.safety_identifier = safety_identifier
        self.thread_number = thread_number
        self.streaming = False
        self.total_conversation_tokens = 0
        self.message_pairs_count = 0
        self.conversation_hash = ""  # Hash of conversation for thread identification
        self.completion_id = ""  # The composite key returned to client
        self.last_summarization_index = -1  # Track last message index that was summarized
        self.summary_token_count = 0  # Token count of current summary


class HandleIdObjectMap:
    """
    A singleton class to manage a mapping between a handle object and Chat ID
    of string type. A handle object needs to be created inorder to start a new chat,
    and one need to provide this handle everytime for asking the query in the same
    conversation. Maximum number of elements supported is @MAX_ELEMENTS.

    If the maximum number of elements reached and a new mapping is added
    the oldest entry is removed to make space.
    """
    _instance = None
    _lock = threading.Lock()

    MAX_ELEMENTS = 50
    MAX_ID_LENGTH = 256

    def __new__(cls, *args, **kwargs):
        if cls._instance is None:
            with cls._lock:
                if cls._instance is None:
                    cls._instance = super().__new__(cls)
                    cls._instance.__init__()
        return cls._instance

    def __init__(self):
        """
        Initializes the mapping and thread counters
        """
        if not hasattr(self, '_initialized'):
            self._handle_mapping: dict[str, Any] = {}
            self._user_thread_counters: dict[str, int] = {}  # Track next thread number per user
            self._lock = threading.Lock()
            self._initialized = True

    def set_handle(self, handle: Any, id: str) -> None:
        """
        Save handle to corresponding id.
        If id already exists in mapping, handle will be updated for corresponding id.

        If adding a new entry exceeds the MAX_ELEMENTS, oldest entry will be deleted
        and new entry will be added.

        Args:
            handle: handle object
            id: A string id of MAX_ID_LENGTH size
        """
        if len(id) > self.MAX_ID_LENGTH:
            raise ValueError(f"ID string {id} cannot exceed {self.MAX_ID_LENGTH} length")

        with self._lock:
            # if max elements reached, then remove oldest entry for adding new entry.
            if len(self._handle_mapping) >= self.MAX_ELEMENTS:
                oldest_entry = next(iter(self._handle_mapping.keys()))

                llm_service = LLMService()
                oldest_handle = self._handle_mapping.get(oldest_entry)

                # Destroy the LLM handle object
                llm_service.lib.llm_destroy_object(oldest_handle)

                # now remove the oldest handle
                removed_handle = self._handle_mapping.pop(oldest_entry)
                logger.debug(f"Max capacity: {self.MAX_ELEMENTS} reached, removed: ID{oldest_entry}: handle: {removed_handle}")

            self._handle_mapping[id] = handle

    def get_handle(self, id: str) -> Any | None:
        """
        Get handle from id saved in mapping.

        Args:
            id: String id of MAX_ID_LENGTH size

        Returns:
            handle object if found, otherwise None
        """
        with self._lock:
            return self._handle_mapping.get(id)

    def delete_handle(self, id: str) -> bool:
        """
        Delete handle mapping using id.

        Args:
            id: String id of MAX_ID_LENGTH size.

        Returns:
            True: If deleted successfully.
            False: Entry not exists.
        """
        with self._lock:
            if id in self._handle_mapping:
                del self._handle_mapping[id]
                return True

            return False

    def get_mapping(self) -> dict[str, Any]:
        """
        Returns a copy of mapping.

        Returns:
            Dictonary containing all mapping.
        """
        with self._lock:
            return self._handle_mapping.copy()

    def get_all_conversation(self) -> list[str]:
        with self._lock:
            return list(self._handle_mapping.keys())

    def get_current_size(self) -> int:
        """
        Returns:
            Current number of mapping stored.
        """
        with self._lock:
            return len(self._handle_mapping)

    def clear_mapping(self) -> None:
        """
        Delete all stored mapping.
        """
        with self._lock:
            self._handle_mapping.clear()
            self._user_thread_counters.clear()

    def get_next_thread_number(self, safety_identifier: str) -> int:
        """
        Get and increment thread counter for a specific user.

        Args:
            safety_identifier: User identifier

        Returns:
            int: The next available thread number for this user
        """
        with self._lock:
            current = self._user_thread_counters.get(safety_identifier, 0)
            self._user_thread_counters[safety_identifier] = current + 1
            logger.debug(f"Assigned thread number {current} to user {safety_identifier}")
            return current

    def get_user_threads(self, safety_identifier: str) -> list[tuple[str, Any]]:
        """
        Get all thread keys and handle objects for a specific user.

        Args:
            safety_identifier: User identifier

        Returns:
            List of tuples (composite_key, handle_object)
        """
        with self._lock:
            prefix = f"{safety_identifier}:thread_"
            user_threads = [(k, v) for k, v in self._handle_mapping.items()
                           if k.startswith(prefix)]
            logger.debug(f"Found {len(user_threads)} threads for user {safety_identifier}")
            return user_threads
