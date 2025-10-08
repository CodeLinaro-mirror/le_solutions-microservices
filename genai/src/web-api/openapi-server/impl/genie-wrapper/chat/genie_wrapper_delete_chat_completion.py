# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from openapi_server.models.chat_completion_deleted import ChatCompletionDeleted
from openapi_server.models.error import Error
from openapi_server.impl.genie_wrapper.gen_ai_service_singleton import LLMService
from openapi_server.logger.logger_config import LoggerConfig
from openapi_server.impl.genie_wrapper.utils.handle_object_interface import HandleIdObjectMap
from openapi_server.impl.constant import HttpStatusCodes, ErrorMessages, Parameters

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)

class GenieWrapperDeleteChatCompletion:
    @staticmethod
    def delete_chat_completion(completion_id: str) -> ChatCompletionDeleted:
        """
        Invokes genie_wrapper_delete_chat_completion function to delete a chat completion.

        Args:
            completion_id (str): The ID of the chat completion to delete.

        Returns:
            ChatCompletionDeleted: The deleted chat completion.
        """
        # Create LLM service instance
        llm_service = LLMService()
        logger.debug(f"Delete chat - completion_id:{completion_id}")
        logger.debug(f"Delete chat - Getting existing handle for completion_id: {completion_id}")
        map_obj = HandleIdObjectMap()
        handle_obj = map_obj.get_handle(completion_id)
        logger.debug(f"Delete chat - Existing handle: {handle_obj}")
        if handle_obj is None or handle_obj.handle_object is None:
            err = Error(code = f"{HttpStatusCodes.NOT_FOUND}", message=ErrorMessages.COMPLETION_ID_NOT_EXIST, param=Parameters.COMPLETION_ID, type=Parameters.INTERNAL_TYPE)
            return err

        handle = handle_obj.handle_object

        # Call the C function to delete the chat completion
        llm_service.lib.llm_chat_completion_delete(handle)

        # Destroy the LLM object
        llm_service.lib.llm_destroy_object(handle)

        # remove in-memory handle from python
        map_obj = HandleIdObjectMap()
        handle = map_obj.get_handle(completion_id)
        if handle is None:
            err = Error(code = f"{HttpStatusCodes.NOT_FOUND}", message=ErrorMessages.COMPLETION_ID_NOT_EXIST, param=Parameters.COMPLETION_ID, type=Parameters.INTERNAL_TYPE)
            return err

        map_obj.delete_handle(completion_id)

        # Return the deletion confirmation
        res=ChatCompletionDeleted(object="chat.completion.deleted", id=completion_id, deleted=True)

        return res
