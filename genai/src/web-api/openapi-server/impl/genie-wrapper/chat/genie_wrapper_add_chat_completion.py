# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from openapi_server.models.create_chat_completion_request import CreateChatCompletionRequest
from openapi_server.models.create_chat_completion_response import CreateChatCompletionResponse
from openapi_server.models.create_chat_completion_response_choices_inner import ChatCompletionResponseMessage, CreateChatCompletionResponseChoicesInner
from openapi_server.models.create_chat_completion_response_choices_inner_logprobs import CreateChatCompletionResponseChoicesInnerLogprobs
from openapi_server.models.chat_completion_token_logprob_top_logprobs_inner import ChatCompletionTokenLogprobTopLogprobsInner
from openapi_server.models.chat_completion_token_logprob import ChatCompletionTokenLogprob
from openapi_server.models.error import Error
from openapi_server.impl.genie_wrapper.chat.utils.chat_utils import ChatQueryUtils
from openapi_server.logger.logger_config import LoggerConfig
from fastapi import HTTPException
from openapi_server.impl.constant import HttpStatusCodes, ErrorMessages, LLMServiceKeys, Parameters, LLMServiceQueryConstant as QUERY_CONST

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)

class GenieWrapperAddChatCompletion:
    @staticmethod
    def add_chat_completion(completion_id: str, request_data: CreateChatCompletionRequest) -> CreateChatCompletionResponse:
        """
        Converts the Python object into C structs, invokes corresponding C
        interfaces, and converts the result back to Python.
        Args:
            completion_id (str): The completion ID.
            request_data (CreateChatCompletionRequest): The request data.
        Returns:
            CreateChatCompletionResponse: The response data.
        """
        last_msg = request_data.messages[-1]
        if last_msg.content == None or len(last_msg.content.strip()) == 0:
            raise HTTPException(status_code=HttpStatusCodes.BAD_REQUEST, detail=ErrorMessages.INCORRECT_CONTENT)
        if len(last_msg.content) > QUERY_CONST.MESSAGE_CONTENT_MAX_SIZE:
            err = Error(code = f"{HttpStatusCodes.BAD_REQUEST}", message=ErrorMessages.MAX_CONTENT_EXCEED, param=Parameters.INTERNAL_TYPE, type=Parameters.INTERNAL_TYPE)
            return err

        query_composer = ChatQueryUtils()
        llm_service, handle, query = query_composer.chat_compose_query(request_data, completion_id)
        if type(llm_service) is Error:
            return llm_service

        response = llm_service.ffi.new(LLMServiceKeys.RESPONSE)
        if response == llm_service.ffi.NULL:
            err = Error(code = f"{HttpStatusCodes.INTERNAL_SERVER_ERROR}", message=ErrorMessages.MEM_ALLOCATION_ERR, param=Parameters.INTERNAL_TYPE, type=Parameters.INTERNAL_TYPE)
            return err

        llm_service.lib.llm_chat_completion_create(handle, query, response)

        choice = response.choices[0]
        msg_choice = choice.message

        logger.info(f"choice.role: {llm_service.ffi.string(msg_choice.role).decode('utf-8')}")
        cclprobe_inner = ChatCompletionTokenLogprobTopLogprobsInner(token=LLMServiceKeys.TOKEN, logprob=int(choice.logprobs), bytes=[1])
        clprob = ChatCompletionTokenLogprob(token=LLMServiceKeys.TOKEN, logprob=int(choice.logprobs), bytes=[1], top_logprobs=[cclprobe_inner])
        lprob = CreateChatCompletionResponseChoicesInnerLogprobs(content=[clprob], refusal=[clprob])
        msg = ChatCompletionResponseMessage(role=llm_service.ffi.string(msg_choice.role).decode('utf-8'),content=llm_service.ffi.string(msg_choice.content).decode('utf-8'),refusal=LLMServiceKeys.REFUSE)
        ccir = CreateChatCompletionResponseChoicesInner(finish_reason=LLMServiceKeys.FINISH_REASON_DEFAULT_VAL,index=choice.index,message=msg,logprobs=lprob)
        res = CreateChatCompletionResponse(id=completion_id,choices=[ccir],created=int(response.created),object=LLMServiceKeys.CHAT_OBJECT_DEFAULT_VAL)

        return res
