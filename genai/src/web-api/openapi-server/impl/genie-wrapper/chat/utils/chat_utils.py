# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from openapi_server.models.create_chat_completion_request import CreateChatCompletionRequest
from openapi_server.models.error import Error
from openapi_server.impl.genie_wrapper.gen_ai_service_singleton import LLMService
from openapi_server.logger.logger_config import LoggerConfig
from openapi_server.impl.genie_wrapper.utils.common_utils import CommonUtils
from openapi_server.impl.genie_wrapper.utils.handle_object_interface import HandleIdObjectMap
from openapi_server.impl.constant import HttpStatusCodes
from openapi_server.impl.constant import ErrorMessages, Parameters, LLMServiceKeys, LLMServiceQueryConstant as QUERY_CONST
LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)

from fastapi import (
    HTTPException
)

class ChatQueryUtils:
    @staticmethod
    def chat_compose_query(request_data: CreateChatCompletionRequest, completion_id: str = None ):
        """
        Executes a chat query using the provided request data.

        Args:
            request_data (CreateChatCompletionRequest): The request data containing the chat query.
            completion_id (str, optional): The completion ID. Defaults to None.

        Returns:
            tuple: A tuple containing the error (if any), the handle, and the query.

        """
        if not request_data.messages:
            raise ValueError("No messages provided in request")

        last_msg = request_data.messages[-1]
        if not last_msg.content or not last_msg.content.strip():
            raise HTTPException(status_code=HttpStatusCodes.BAD_REQUEST, detail=ErrorMessages.INCORRECT_CONTENT)

        model_str = str(CommonUtils.get_model_name())
        logger.debug(f"Value of model_str: {model_str}")
        llm_service = LLMService()
        handle = None
        query = None
        if completion_id:
            # get the existing handle
            logger.debug(f"Getting existing handle for completion_id: {completion_id}")
            map_obj = HandleIdObjectMap()
            handle_obj = map_obj.get_handle(completion_id)
            logger.debug(f"Existing handle: {handle_obj}")
            if handle_obj is None or handle_obj.handle_object is None:
                err = Error(code = f"{HttpStatusCodes.NOT_FOUND}", message=ErrorMessages.COMPLETION_ID_NOT_EXIST, param=Parameters.COMPLETION_ID, type=Parameters.INTERNAL_TYPE)
                return err, handle, query

            if handle_obj.messages_count >= QUERY_CONST.MAX_MESSAGE_COUNT:
                err = Error(code = f"{HttpStatusCodes.BAD_REQUEST}", message=ErrorMessages.MAX_MSG_LIMIT_REACHED, param=Parameters.COMPLETION_ID, type=Parameters.INTERNAL_TYPE)
                return err, handle, query

            handle = handle_obj.handle_object
        else:

            model_input = llm_service.ffi.new("char[]", model_str.encode('utf-8'))
            if model_input == llm_service.ffi.NULL:
                logger.error("Failed to allocate Model pointer.")
                err = Error(code = f"{HttpStatusCodes.INTERNAL_SERVER_ERROR}", message=ErrorMessages.MEM_ALLOCATION_ERR, param=Parameters.INTERNAL_TYPE, type=Parameters.INTERNAL_TYPE)
                return err, handle, query

            # Add check for existing chat-id for existing handle
            map_obj = HandleIdObjectMap()
            if map_obj.get_current_size() > 0:
                conv_ids = map_obj.get_all_conversation()
                print(conv_ids)
                conv_id = conv_ids[0]
                logger.error("Existing conversation with chat-id")
                err = Error(code = f"{HttpStatusCodes.CONFLICT}", message=ErrorMessages.CHAT_ID_EXISTS + conv_id, param=Parameters.INTERNAL_TYPE, type=Parameters.INTERNAL_TYPE)
                return err, handle, query

            handle = llm_service.lib.llm_create_object(model_input)
            if handle == llm_service.ffi.NULL:
                logger.error("Failed to create LLM object: received NULL pointer.")
                err = Error(code = f"{HttpStatusCodes.INTERNAL_SERVER_ERROR}", message=ErrorMessages.OBJ_CREATION_FAILED, param=Parameters.LLM_OBJECT, type=Parameters.INTERNAL_TYPE)
                return err, handle, query

        query = llm_service.ffi.new(LLMServiceKeys.QUERY)
        if query == llm_service.ffi.NULL:
            logger.error("Failed to allocate Query pointer.")
            err = Error(code = f"{HttpStatusCodes.INTERNAL_SERVER_ERROR}", message=ErrorMessages.MEM_ALLOCATION_ERR, param=Parameters.INTERNAL_TYPE, type=Parameters.INTERNAL_TYPE)
            return err, handle, query

        """
        query.frequency_penalty = int(request_data.frequency_penalty or QUERY_CONST.DEFAULT_FREQUENCY_PENALTY)
        query.logprobs = bool(request_data.logprobs)
        query.n = int(request_data.n or QUERY_CONST.DEFAULT_N)
        query.parallel_tool_calls = bool(request_data.parallel_tool_calls)
        query.presence_penalty = int(request_data.presence_penalty or QUERY_CONST.DEFAULT_PRESENCE_PENALTY)
        query.stream = bool(request_data.stream)
        query.top_logprobs = int(request_data.top_logprobs or QUERY_CONST.DEFAULT_TOP_LOGPROBS)
        query.max_completion_tokens = int(request_data.max_completion_tokens or 0)
        query.temperature = float(request_data.temperature or QUERY_CONST.DEFAULT_TEMPERATURE)
        query.top_k = QUERY_CONST.DEFAULT_TOP_K
        query.top_p = float(request_data.top_p or QUERY_CONST.DEFAULT_TOP_P)
        """
        CommonUtils.copy_py_string_to_c_array(llm_service.ffi, query.model,
                        model_str, QUERY_CONST.MODEL_STR_MAX_SIZE)
        CommonUtils.copy_py_string_to_c_array(llm_service.ffi, query.message.role,
                        last_msg.role, QUERY_CONST.ROLE_MAX_SIZE)
        CommonUtils.copy_py_string_to_c_array(llm_service.ffi, query.message.content,
                        last_msg.content, QUERY_CONST.MESSAGE_CONTENT_MAX_SIZE)

        return llm_service, handle, query

    @staticmethod
    def chat_increment_message_count(completion_id: str = None):
        """
        increment the msg count for given chat id
        """
        map_obj = HandleIdObjectMap()
        handle_obj = map_obj.get_handle(completion_id)
        if handle_obj:
            handle_obj.messages_count = handle_obj.messages_count + 1
            map_obj.set_handle(handle_obj, completion_id)

