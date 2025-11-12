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
from openapi_server.impl.genie_wrapper.utils.handle_object_interface import HandleIdObjectMap, HandleObject
from fastapi import HTTPException
from openapi_server.impl.constant import HttpStatusCodes, LLMServiceKeys, ErrorMessages, Parameters, LLMServiceQueryConstant as QUERY_CONST, APIResponseKeys as RESPNS_KEYS
import uuid
from fastapi.responses import StreamingResponse
from queue import Queue
import threading
import asyncio
from typing import Union
import json

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)

class GenieWrapperCreateChatCompletion:

    def __generate_chat_id():
        """Returns the current timestamp in milliseconds as a string."""
        return f"chat-{uuid.uuid4()}"

    @staticmethod
    def create_chat_completion(request_data: CreateChatCompletionRequest) -> Union[StreamingResponse, CreateChatCompletionResponse]:
        last_msg = request_data.messages[-1]
        if not last_msg.content or not last_msg.content.strip():
            raise HTTPException(status_code=HttpStatusCodes.BAD_REQUEST, detail=ErrorMessages.INCORRECT_CONTENT)

        if len(last_msg.content) > QUERY_CONST.MESSAGE_CONTENT_MAX_SIZE:
            return Error(
                code=f"{HttpStatusCodes.BAD_REQUEST}",
                message=ErrorMessages.MAX_CONTENT_EXCEED,
                param=Parameters.INTERNAL_TYPE,
                type=Parameters.INTERNAL_TYPE
            )

        query_composer = ChatQueryUtils()
        llm_service, handle, query = query_composer.chat_compose_query(request_data)

        if isinstance(llm_service, Error):
            return llm_service

        q = Queue()

        @llm_service.ffi.callback("void(const Response *)")
        def callback(response_ptr):
            resp = response_ptr[0]
            choice = resp.choices[0]
            msg = choice.message

            content = llm_service.ffi.string(msg.content).decode("utf-8")
            finish_reason = llm_service.ffi.string(choice.finish_reason).decode("utf-8")

            # Push structured response
            q.put({
                RESPNS_KEYS.CHAT_ID: llm_service.ffi.string(resp.id).decode("utf-8"),
                RESPNS_KEYS.CHAT_OBJECT: 'chat.completion',
                RESPNS_KEYS.CHAT_CREATED: resp.created,
                RESPNS_KEYS.MODEL: llm_service.ffi.string(query.model).decode("utf-8"),
                RESPNS_KEYS.MESSAGE_ROLE: llm_service.ffi.string(msg.role).decode("utf-8"),
                RESPNS_KEYS.MESSAGE_CONTENT: content,
                RESPNS_KEYS.CHOICE_FINISH_REASON: finish_reason
            })

            # Signal end of stream if finish_reason is "stop" and content is empty
            if request_data.stream and finish_reason == "stop" and content == "":
                q.put("[DONE]")

        def run_llm():
            try:
                llm_service.lib.llm_chat_completion_create(handle, query, request_data.stream, callback)
            except Exception as e:
                logger.error(f"Error in C callback execution: {e}")
                q.put({"error": str(e)})
            finally:
                if not request_data.stream:
                    q.put("[DONE]")

        threading.Thread(target=run_llm, daemon=True).start()

        if request_data.stream:
            async def stream_generator():
                loop = asyncio.get_event_loop()
                first_chunk_sent = False
                key = ""
                while True:
                    item = await loop.run_in_executor(None, q.get)

                    if item == "[DONE]":
                        yield "data: [DONE]\n\n"
                        break
                    elif isinstance(item, dict) and "error" in item:
                        yield f"data: [ERROR] {item['error']}\n\n"
                        break
                    elif isinstance(item, dict):
                        chunk = {
                            "id": key,
                            "object": "chat.completion.chunk",
                            "model": item["model"],
                            "created": item["created"],
                            "choices": [{
                                "delta": {},
                                "index": 0,
                                "finish_reason": None
                            }]
                        }

                        if not first_chunk_sent:
                            if len(request_data.messages) == 1:
                                key = GenieWrapperCreateChatCompletion.__generate_chat_id()
                                chunk["id"] = key
                                map_obj = HandleIdObjectMap()
                                handle_obj = HandleObject(handle)
                                handle_obj.streaming = True
                                map_obj.set_handle(handle_obj, key)
                            else:
                                map_obj = HandleIdObjectMap()
                                conv_ids = map_obj.get_all_conversation()
                                chunk["id"] = conv_ids[0]
                            chunk["choices"][0]["delta"]["role"] = item["role"]
                            first_chunk_sent = True

                        if item["content"]:
                            chunk["choices"][0]["delta"]["content"] = item["content"]

                        if item["finish_reason"] == "stop" and not item["content"]:
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

            msg = ChatCompletionResponseMessage(
                role=item["role"],
                content=item["content"],
                refusal=LLMServiceKeys.REFUSE
            )

            ccir = CreateChatCompletionResponseChoicesInner(
                finish_reason='stop',
                index=0,
                message=msg,
                logprobs=None
            )

            key = item["id"]
            if not key and len(request_data.messages) == 1:
                key = GenieWrapperCreateChatCompletion.__generate_chat_id()
                map_obj = HandleIdObjectMap()
                handle_obj = HandleObject(handle)
                handle_obj.streaming = False
                map_obj.set_handle(handle_obj, key)
            else:
                map_obj = HandleIdObjectMap()
                conv_ids = map_obj.get_all_conversation()
                key = conv_ids[0]

            return CreateChatCompletionResponse(
                id=key,
                choices=[ccir],
                created=int(item["created"]),
                model=item[RESPNS_KEYS.MODEL],
                object=item[RESPNS_KEYS.CHAT_OBJECT]
            )
