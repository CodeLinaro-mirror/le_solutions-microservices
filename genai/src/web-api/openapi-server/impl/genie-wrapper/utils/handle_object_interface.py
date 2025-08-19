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
    A class represents object to be held by the map represented by class HandleIdObjectMap

    """
    handle_object = None
    messages_count = 0

    def __init__(self, handle_obj:Any, msg_count: int = 1):
        """
        Initializes an instance of HandleObject.

        Args:
            handle_object: Can be of any data type.
            chat_count (int): Must be an integer.
        """
        self.handle_object = handle_obj
        self.messages_count = msg_count


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
        Initializes the mapping
        """
        if not hasattr(self, '_initialized'):
            self._handle_mapping: dict[str, Any] = {}
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

                # Call the C function to delete the chat history associated with handel
                llm_service.lib.llm_chat_completion_delete(oldest_handle)

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
