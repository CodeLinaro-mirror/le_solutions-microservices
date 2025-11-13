# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import os
import threading
import uuid
from queue import Empty # For non-asyncio.Queue specific usage if needed
from typing import Union, Dict, Any
from queue import Queue

from cffi import FFI
from fastapi import HTTPException
from fastapi.responses import StreamingResponse # Keep for type hinting for now, though not used for streaming response

from openapi_server.models.error import Error
from openapi_server.impl.constant import  EnvVariableKeys
from openapi_server.impl.genie_wrapper.utils.common_utils import CommonUtils
from openapi_server.impl.genie_wrapper.utils.image_validator import decode_image
from openapi_server.impl.genie_wrapper.utils.image_preprocessor import preprocess_from_decoded
from openapi_server.models.create_chat_completion_request import CreateChatCompletionRequest
from openapi_server.models.create_chat_completion_response import CreateChatCompletionResponse
from openapi_server.models.create_chat_completion_response_choices_inner import ChatCompletionResponseMessage, CreateChatCompletionResponseChoicesInner
from openapi_server.models.create_chat_completion_response_choices_inner_logprobs import CreateChatCompletionResponseChoicesInnerLogprobs
from openapi_server.models.chat_completion_token_logprob_top_logprobs_inner import ChatCompletionTokenLogprobTopLogprobsInner
from openapi_server.models.chat_completion_token_logprob import ChatCompletionTokenLogprob
from openapi_server.models.error import Error
from openapi_server.impl.genie_wrapper.chat.utils.chat_utils import ChatQueryUtils
from openapi_server.impl.genie_wrapper.chat.utils.tool_handler import ToolHandler
from openapi_server.impl.genie_wrapper.chat.genie_wrapper_create_vlm_chat_completion import GenieWrapperCreateVLMChatCompletion
from openapi_server.impl.genie_wrapper.utils.image_validator import has_image_content
from openapi_server.impl.model_config_manager import ModelConfigManager
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
    async def create_chat_completion(request_data: CreateChatCompletionRequest, raw_json: dict = None, fastapi_request = None) -> Union[StreamingResponse, CreateChatCompletionResponse]:
        # FIX: Extract raw messages from JSON to bypass broken Pydantic OneOf deserialization
        logger.info("=== IMAGE DETECTION USING RAW JSON ===")

        if raw_json:
            raw_messages = raw_json.get('messages', [])
            logger.info(f"Using raw JSON messages for image detection (bypassing Pydantic OneOf)")
            logger.info(f"Raw messages count: {len(raw_messages)}")

            # Log content structure for debugging
            for idx, msg in enumerate(raw_messages):
                if isinstance(msg, dict):
                    content = msg.get('content')
                    if isinstance(content, list):
                        logger.info(f"Message {idx}: multimodal content with {len(content)} items")
                        for item_idx, item in enumerate(content):
                            if isinstance(item, dict):
                                logger.info(f"  Item {item_idx}: type={item.get('type')}")
                    else:
                        logger.info(f"Message {idx}: text-only content")
        else:
            raw_messages = []
            logger.warning("No raw JSON provided - falling back to Pydantic models (may fail for images)")

        # Check if request contains images - route to VLM if so
        if has_image_content(raw_messages):
            logger.info("✓ IMAGE CONTENT DETECTED - Routing to VLM handler")

            # Validate model supports vision
            requested_model = getattr(request_data, 'model', None)
            if requested_model:
                config_manager = ModelConfigManager()
                if not config_manager.supports_vision(requested_model):
                    raise HTTPException(
                        status_code=HttpStatusCodes.BAD_REQUEST,
                        detail=f"Model '{requested_model}' does not support vision/image inputs. Please use a vision-enabled model."
                    )
            else:
                raise HTTPException(
                    status_code=HttpStatusCodes.BAD_REQUEST,
                    detail="Model must be specified for requests with images"
                )

            # Route to VLM handler - pass raw_json for image extraction
            return await GenieWrapperCreateVLMChatCompletion.create_vlm_chat_completion(request_data, raw_json)

        # No images - proceed with LLM handler
        logger.info("✗ NO IMAGE CONTENT DETECTED - Using LLM handler")

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

        # Handle function calling if tools are provided
        has_tools = request_data.tools and len(request_data.tools) > 0
        modified_request = request_data

        if has_tools:
            logger.info(f"Function calling enabled with {len(request_data.tools)} tools")
            # Create a deep copy to avoid modifying the original request
            modified_request = request_data.model_copy(deep=True)
            modified_request.messages = ToolHandler.inject_tool_instructions(
                modified_request.messages,
                request_data.tools
            )

        query_composer = ChatQueryUtils()
        llm_service, handle, query, completion_id = query_composer.chat_compose_query(modified_request)

        if isinstance(llm_service, Error):
            return llm_service

        logger.info(f"Received completion_id from chat_compose_query: {completion_id}")

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
                accumulated_content = ""  # Accumulate content to check for tool calls

                while True:
                    item = await loop.run_in_executor(None, q.get)

                    if item == "[DONE]":
                        # Before finishing, check if accumulated content contains tool calls
                        if has_tools and accumulated_content and ToolHandler.should_check_for_tools(accumulated_content):
                            detected_tool_calls = ToolHandler.parse_tool_response(accumulated_content)
                            if detected_tool_calls:
                                logger.info(f"Detected {len(detected_tool_calls)} tool call(s) in streaming response")
                                # Send tool calls chunk
                                tool_chunk = {
                                    "id": key,
                                    "object": "chat.completion.chunk",
                                    "model": modified_request.model,  # Use model from request
                                    "created": int(asyncio.get_event_loop().time()),  # Use current timestamp
                                    "choices": [{
                                        "delta": {
                                            "tool_calls": [
                                                {
                                                    "index": idx,
                                                    "id": tc.id,
                                                    "type": tc.type,
                                                    "function": {
                                                        "name": tc.function.name,
                                                        "arguments": tc.function.arguments
                                                    }
                                                } for idx, tc in enumerate(detected_tool_calls)
                                            ]
                                        },
                                        "index": 0,
                                        "finish_reason": "tool_calls"
                                    }]
                                }
                                yield f"data: {json.dumps(tool_chunk)}\n\n"

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
                            # Use the completion_id returned from chat_compose_query
                            key = completion_id
                            chunk["id"] = key
                            chunk["choices"][0]["delta"]["role"] = "assistant"  # Response role is always assistant
                            first_chunk_sent = True

                        if item["content"]:
                            accumulated_content += item["content"]
                            chunk["choices"][0]["delta"]["content"] = item["content"]

                        if item["finish_reason"] == "stop" and not item["content"]:
                            chunk["choices"][0]["finish_reason"] = "stop"

                        yield f"data: {json.dumps(chunk)}\n\n"

            return StreamingResponse(stream_generator(), media_type="text/event-stream")

        else:
            # Non-streaming response
            loop = asyncio.get_event_loop()
            item = await loop.run_in_executor(None, q.get)
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

            content = item["content"]
            finish_reason = 'stop'
            tool_calls = None

            # Check for tool calls if tools were provided
            if has_tools and content and ToolHandler.should_check_for_tools(content):
                detected_tool_calls = ToolHandler.parse_tool_response(content)
                if detected_tool_calls:
                    logger.info(f"Detected {len(detected_tool_calls)} tool call(s) in response")
                    tool_calls = detected_tool_calls
                    content = None  # Clear content when returning tool calls
                    finish_reason = 'tool_calls'

            msg = ChatCompletionResponseMessage(
                role="assistant",  # Response messages are always from assistant
                content=content,
                refusal=LLMServiceKeys.REFUSE,
                tool_calls=tool_calls
            )

            ccir = CreateChatCompletionResponseChoicesInner(
                finish_reason=finish_reason,
                index=0,
                message=msg,
                logprobs=None
            )

            # Use the completion_id returned from chat_compose_query
            key = completion_id

            return CreateChatCompletionResponse(
                id=key,
                choices=[ccir],
                created=int(item["created"]),
                model=item[RESPNS_KEYS.MODEL],
                object=item[RESPNS_KEYS.CHAT_OBJECT]
            )
