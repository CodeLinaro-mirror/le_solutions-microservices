# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from openapi_server.models.create_completion_request import CreateCompletionRequest
from openapi_server.models.create_completion_response import CreateCompletionResponse
from openapi_server.models.error import Error
from openapi_server.models.create_completion_response_choices_inner import CreateCompletionResponseChoicesInner
from openapi_server.models.create_completion_response_choices_inner import CreateCompletionResponseChoicesInner
from openapi_server.impl.genie_wrapper.gen_ai_service_singleton import LLMService
from openapi_server.impl.genie_wrapper.utils.common_utils import CommonUtils
from openapi_server.logger.logger_config import LoggerConfig
from openapi_server.impl.constant import LLMServiceKeys, Parameters, HttpStatusCodes, ErrorMessages, LLMServiceQueryConstant as QUERY_CONST
from openapi_server.impl.genie_wrapper.utils.handle_object_interface import HandleIdObjectMap
from fastapi import HTTPException

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)

class GenieWrapperCreateCompletion:
    @staticmethod
    def create_completion(create_completion_request: CreateCompletionRequest) -> CreateCompletionResponse:
        """
        This function converts the python object into C structs, invokes corresponding C
        intrfaces and converts the result back to python.
        Invoke the C function to create a completion.
        Args:
            create_completion_request (CreateCompletionRequest): The request object containing the prompt and other parameters.
        Returns:
            CreateCompletionResponse: The response object containing the generated completion.
        """

        # Add check for existing chat-id for existing handle
        map_obj = HandleIdObjectMap()
        if map_obj.get_current_size() > 0:
            conv_ids = map_obj.get_all_conversation()
            print(conv_ids)
            conv_id = conv_ids[0]
            logger.error("Existing conversation with chat-id")
            err = Error(code = f"{HttpStatusCodes.CONFLICT}", message=ErrorMessages.CHAT_ID_EXISTS + conv_id, param=Parameters.INTERNAL_TYPE, type=Parameters.INTERNAL_TYPE)
            return err

        if len(create_completion_request.prompt.strip()) == 0:
            raise HTTPException(status_code=HttpStatusCodes.BAD_REQUEST, detail=ErrorMessages.INCORRECT_CONTENT)
        if len(create_completion_request.prompt) > QUERY_CONST.MESSAGE_CONTENT_MAX_SIZE:
            err = Error(code = f"{HttpStatusCodes.BAD_REQUEST}", message=ErrorMessages.MAX_CONTENT_EXCEED, param=Parameters.INTERNAL_TYPE, type=Parameters.INTERNAL_TYPE)
            return err
        
        model_str = str(CommonUtils.get_model_name())

        llm_service = LLMService()
        model_input = llm_service.ffi.new("char[]", model_str.encode('utf-8'))
        if model_input == llm_service.ffi.NULL:
                logger.error("Failed to allocate model pointer.")
                err = Error(code = f"{HttpStatusCodes.INTERNAL_SERVER_ERROR}", message=ErrorMessages.MEM_ALLOCATION_ERR, param=Parameters.INTERNAL_TYPE, type=Parameters.INTERNAL_TYPE)
                return err

        handle = llm_service.lib.llm_create_object(model_input)
        if handle == llm_service.ffi.NULL:
            logger.error(f"Failed to create LLM object: received NULL pointer.")
            return {"error": "LLM object creation failed"}
        # Prepare the Query struct
        query = llm_service.ffi.new(LLMServiceKeys.QUERY)
        if query == llm_service.ffi.NULL:
            logger.error("Failed to allocate Query pointer.")
            err = Error(code = f"{HttpStatusCodes.INTERNAL_SERVER_ERROR}", message=ErrorMessages.MEM_ALLOCATION_ERR, param=Parameters.INTERNAL_TYPE, type=Parameters.INTERNAL_TYPE)
            return err

        # Fill message.role and message.content and model

        CommonUtils.copy_py_string_to_c_array(llm_service.ffi, query.model,
                        model_str, QUERY_CONST.MODEL_STR_MAX_SIZE)
        CommonUtils.copy_py_string_to_c_array(llm_service.ffi, query.message.role,
                        QUERY_CONST.DEFAULT_ROLE, QUERY_CONST.ROLE_MAX_SIZE)
        CommonUtils.copy_py_string_to_c_array(llm_service.ffi, query.message.content,
                        create_completion_request.prompt, QUERY_CONST.MESSAGE_CONTENT_MAX_SIZE)


        """
        # Fill numeric fields
        query.max_completion_tokens = create_completion_request.max_tokens or QUERY_CONST.DEFAULT_COMPLETION_TOKEN
        query.temperature = float(create_completion_request.temperature or QUERY_CONST.DEFAULT_TEMPERATURE)
        query.top_k = QUERY_CONST.DEFAULT_TOP_K  # Not provided, default to 0
        query.top_p = float(create_completion_request.top_p or QUERY_CONST.DEFAULT_TOP_P)

        # Set unsupported fields to defaults
        query.frequency_penalty = QUERY_CONST.DEFAULT_FREQUENCY_PENALTY
        query.presence_penalty = QUERY_CONST.DEFAULT_PRESENCE_PENALTY
        query.stream = QUERY_CONST.DEFAULT_STREAM
        query.logprobs = QUERY_CONST.DEFAULT_LOGPROBS
        query.n = QUERY_CONST.DEFAULT_N
        query.parallel_tool_calls = QUERY_CONST.DEFAULT_PARALLEL_TOOL_CALLS
        """
        # Prepare the Response struct
        response = llm_service.ffi.new(LLMServiceKeys.RESPONSE)
        if response == llm_service.ffi.NULL:
            err = Error(code = f"{HttpStatusCodes.INTERNAL_SERVER_ERROR}", message=ErrorMessages.MEM_ALLOCATION_ERR, param=Parameters.INTERNAL_TYPE, type=Parameters.INTERNAL_TYPE)
            return err

        # Call the C function
        logger.info("Making query call to C interface")
        llm_service.lib.llm_chat_completion_create(handle, query, response)

        # Call the C function to delete the chat completion
        llm_service.lib.llm_chat_completion_delete(handle)

        # Destroy the LLM object
        llm_service.lib.llm_destroy_object(handle)

        # Extract choices
        #for i in range(10):  # MAX_CHOICES
        choice = response.choices[0]
        msg_choice = choice.message

        logger.info(f"choice.rrole: {llm_service.ffi.string(msg_choice.role).decode('utf-8')}")
        ccir = CreateCompletionResponseChoicesInner(finish_reason=LLMServiceKeys.FINISH_REASON_DEFAULT_VAL, index=choice.index, logprobs=None, text=llm_service.ffi.string(msg_choice.content).decode('utf-8'))
        response.object = LLMServiceKeys.RESPONSE_OBJ_TEXT_COMPLETION.encode("utf-8")
        res = CreateCompletionResponse(id = llm_service.ffi.string(response.id).decode('utf-8'), choices=[ccir], created=int(response.created), model=QUERY_CONST.DEFAULT_MODEL, object=llm_service.ffi.string(response.object).decode('utf-8'))

        return res
