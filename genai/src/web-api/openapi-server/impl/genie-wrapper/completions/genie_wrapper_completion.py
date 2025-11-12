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
from openapi_server.impl.constant import LLMServiceKeys, Parameters, HttpStatusCodes, ErrorMessages, LLMServiceQueryConstant as QUERY_CONST, APIResponseKeys as RESPNS_KEYS
from openapi_server.impl.genie_wrapper.utils.handle_object_interface import HandleIdObjectMap
from fastapi import HTTPException
from fastapi.responses import StreamingResponse
from queue import Queue
import threading
import asyncio
from typing import Union
import json

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
        # Check for existing chat-id
        map_obj = HandleIdObjectMap()
        if map_obj.get_current_size() > 0:
            conv_ids = map_obj.get_all_conversation()
            conv_id = conv_ids[0]
            logger.error("Existing conversation with chat-id")
            return Error(
                code=f"{HttpStatusCodes.CONFLICT}",
                message=ErrorMessages.CHAT_ID_EXISTS + conv_id,
                param=Parameters.INTERNAL_TYPE,
                type=Parameters.INTERNAL_TYPE
            )

        # Validate prompt
        if not create_completion_request.prompt or not create_completion_request.prompt.strip():
            raise HTTPException(status_code=HttpStatusCodes.BAD_REQUEST, detail=ErrorMessages.INCORRECT_CONTENT)

        if len(create_completion_request.prompt) > QUERY_CONST.MESSAGE_CONTENT_MAX_SIZE:
            return Error(
                code=f"{HttpStatusCodes.BAD_REQUEST}",
                message=ErrorMessages.MAX_CONTENT_EXCEED,
                param=Parameters.INTERNAL_TYPE,
                type=Parameters.INTERNAL_TYPE
            )

        # Prepare model and query
        model_str = str(CommonUtils.get_model_name())
        llm_service = LLMService()
        model_input = llm_service.ffi.new("char[]", model_str.encode("utf-8"))

        if model_input == llm_service.ffi.NULL:
            logger.error("Failed to allocate model pointer.")
            return Error(
                code=f"{HttpStatusCodes.INTERNAL_SERVER_ERROR}",
                message=ErrorMessages.MEM_ALLOCATION_ERR,
                param=Parameters.INTERNAL_TYPE,
                type=Parameters.INTERNAL_TYPE
            )

        handle = llm_service.lib.llm_create_object(model_input, create_completion_request.stream)
        if handle == llm_service.ffi.NULL:
            logger.error("Failed to create LLM object.")
            return Error(
                code=f"{HttpStatusCodes.INTERNAL_SERVER_ERROR}",
                message="LLM object creation failed",
                param=Parameters.INTERNAL_TYPE,
                type=Parameters.INTERNAL_TYPE
            )

        query = llm_service.ffi.new(LLMServiceKeys.QUERY)
        if query == llm_service.ffi.NULL:
            logger.error("Failed to allocate Query pointer.")
            return Error(
                code=f"{HttpStatusCodes.INTERNAL_SERVER_ERROR}",
                message=ErrorMessages.MEM_ALLOCATION_ERR,
                param=Parameters.INTERNAL_TYPE,
                type=Parameters.INTERNAL_TYPE
            )

        # Fill query fields
        CommonUtils.copy_py_string_to_c_array(llm_service.ffi, query.model, model_str, QUERY_CONST.MODEL_STR_MAX_SIZE)
        CommonUtils.copy_py_string_to_c_array(llm_service.ffi, query.message.role, QUERY_CONST.DEFAULT_ROLE, QUERY_CONST.ROLE_MAX_SIZE)
        CommonUtils.copy_py_string_to_c_array(llm_service.ffi, query.message.content, create_completion_request.prompt, QUERY_CONST.MESSAGE_CONTENT_MAX_SIZE)

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
        # Prepare queue and callback
        q = Queue()

        @llm_service.ffi.callback("void(const Response *)")
        def callback(response_ptr):
            resp = response_ptr[0]
            choice = resp.choices[0]
            msg = choice.message

            content = llm_service.ffi.string(msg.content).decode("utf-8")
            finish_reason = llm_service.ffi.string(choice.finish_reason).decode("utf-8")

            q.put({
                RESPNS_KEYS.CHAT_ID: llm_service.ffi.string(resp.id).decode("utf-8"),
                RESPNS_KEYS.CHAT_OBJECT: llm_service.ffi.string(resp.object).decode('utf-8'),
                RESPNS_KEYS.CHAT_CREATED: resp.created,
                RESPNS_KEYS.MODEL: llm_service.ffi.string(resp.model).decode("utf-8"),
                RESPNS_KEYS.MESSAGE_CONTENT: content,
                RESPNS_KEYS.CHOICE_FINISH_REASON: finish_reason
            })

            if create_completion_request.stream and finish_reason == "stop" and content == "":
                q.put("[DONE]")

        def run_llm():
            try:
                llm_service.lib.llm_chat_completion_create(handle, query, create_completion_request.stream, callback)
            except Exception as e:
                logger.error(f"Error in C callback execution: {e}")
                q.put({"error": str(e)})
            finally:
                if not create_completion_request.stream:
                    q.put("[DONE]")
                llm_service.lib.llm_destroy_object(handle)

        threading.Thread(target=run_llm, daemon=True).start()

        if create_completion_request.stream:
            async def stream_generator():
                loop = asyncio.get_event_loop()
                first_chunk_sent = False

                while True:
                    item = await loop.run_in_executor(None, q.get)

                    if item == "[DONE]":
                        yield "data: [DONE]\n\n"
                        break
                    elif isinstance(item, dict) and "error" in item:
                        yield f"data: [ERROR] {item['error']}\n\n"
                        break
                    elif isinstance(item, dict):
                        content = item.get(RESPNS_KEYS.MESSAGE_CONTENT, "")
                        if not content:
                            continue

                        chunk = {
                            "id": item[RESPNS_KEYS.CHAT_ID],
                            "object": "text_completion",
                            "model": item[RESPNS_KEYS.MODEL],
                            "created": item[RESPNS_KEYS.CHAT_CREATED],
                            "choices": [{
                                "text": content,
                                "index": 0,
                                "finish_reason": None
                            }]
                        }

                        if item[RESPNS_KEYS.CHOICE_FINISH_REASON] == "stop" and not content:
                            chunk["choices"][0]["finish_reason"] = "stop"

                        yield f"data: {json.dumps(chunk)}\n\n"

            return StreamingResponse(stream_generator(), media_type="text/event-stream")

        else:
            # Non-streaming response
            item = q.get()
            if item == "[DONE]":
                return Error(
                    code=f"{HttpStatusCodes.INTERNAL_SERVER_ERROR}",
                    message="Empty response received",
                    param=Parameters.INTERNAL_TYPE,
                    type=Parameters.INTERNAL_TYPE
                )
            elif isinstance(item, dict) and "error" in item:
                return Error(
                    code=f"{HttpStatusCodes.INTERNAL_SERVER_ERROR}",
                    message=item["error"],
                    param=Parameters.INTERNAL_TYPE,
                    type=Parameters.INTERNAL_TYPE
                )

            ccir = CreateCompletionResponseChoicesInner(
                finish_reason='stop',
                index=0,
                logprobs=None,
                text=item[RESPNS_KEYS.MESSAGE_CONTENT]
            )

            return CreateCompletionResponse(
                id=item[RESPNS_KEYS.CHAT_ID],
                choices=[ccir],
                created=int(item[RESPNS_KEYS.CHAT_CREATED]),
                model=item[RESPNS_KEYS.MODEL],
                object="text_completion"
            )
