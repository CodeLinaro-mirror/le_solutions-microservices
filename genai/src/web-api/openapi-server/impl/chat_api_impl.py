# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from openapi_server.apis.chat_api_base import BaseChatApi
from openapi_server.models.create_chat_completion_request import CreateChatCompletionRequest
from openapi_server.models.create_chat_completion_response import CreateChatCompletionResponse
from openapi_server.models.chat_completion_deleted import ChatCompletionDeleted
from openapi_server.models.error import Error
from openapi_server.impl.genie_wrapper.chat.genie_wrapper_create_chat_completion import GenieWrapperCreateChatCompletion
from openapi_server.impl.genie_wrapper.chat.genie_wrapper_add_chat_completion import GenieWrapperAddChatCompletion
from openapi_server.impl.genie_wrapper.chat.utils.chat_utils import ChatQueryUtils
from openapi_server.logger.logger_config import LoggerConfig
from openapi_server.impl.constant import HttpStatusCodes, ErrorMessages
from openapi_server.impl.model_config_manager import ModelConfigManager

from fastapi import (
    HTTPException
)
from fastapi.responses import JSONResponse, StreamingResponse

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)

class ChatApiImpl(BaseChatApi):
    async def add_chat_completion(
        self,
        completion_id: str,
        create_chat_completion_request: CreateChatCompletionRequest
    ) -> CreateChatCompletionResponse:
        """
        Adds a chat to an existing conversation.
        Args:
            completion_id (str): The ID of the chat completion to add the chat to.
            create_chat_completion_request (CreateChatCompletionRequest): The chat to add to the chat completion.
        Returns:
            CreateChatCompletionResponse: The updated chat completion.
        Raises:
            HTTPException: If the chat completion is not found or if there is an error adding the chat to the chat completion.

        """
        try:
            # Validate the model if provided in the request
            if create_chat_completion_request.model:
                config_manager = ModelConfigManager()
                if not config_manager.validate_model(create_chat_completion_request.model):
                    error_message = ErrorMessages.MODEL_NOT_FOUND.format(model=create_chat_completion_request.model)
                    logger.error(f"Invalid model requested: {create_chat_completion_request.model}")
                    raise HTTPException(status_code=HttpStatusCodes.BAD_REQUEST, detail=error_message)
                logger.info(f"Model validation passed for: {create_chat_completion_request.model}")

            result = GenieWrapperAddChatCompletion.add_chat_completion(
                completion_id,
                create_chat_completion_request
            )
            if isinstance(result, Error):
                logger.error(f"Expected CreateChatCompletionResponse, got {type(result)}")
                raise HTTPException(status_code=int(result.code), detail=result.message)
            else:
                logger.info(f"add_chat_completion result: {result}")
                # increment the query count
                ChatQueryUtils.chat_increment_message_count(completion_id)
                return result
        except HTTPException as http_exc:
                logger.error(f"HTTPException error in add_chat_completion: {http_exc}")
                raise HTTPException(status_code=http_exc.status_code, detail=http_exc.detail)
        except Exception as e:
            logger.error(f"Unexpected error in add_chat_completion: {e}")
            raise HTTPException(status_code=HttpStatusCodes.INTERNAL_SERVER_ERROR, detail=ErrorMessages.UNEXPECTED_ERROR)

    async def create_chat_completion(
    self,
    create_chat_completion_request: CreateChatCompletionRequest
    ):
        """
        Creates a new chat conversation.
        Args:
            create_chat_completion_request (CreateChatCompletionRequest): The chat completion to create.
        Returns:
            StreamingResponse or JSONResponse: The created chat completion.
        Raises:
            HTTPException: If there is an error creating the chat completion.
        """
        try:
            # Validate the model if provided in the request
            if create_chat_completion_request.model:
                config_manager = ModelConfigManager()
                if not config_manager.validate_model(create_chat_completion_request.model):
                    error_message = ErrorMessages.MODEL_NOT_FOUND.format(model=create_chat_completion_request.model)
                    logger.error(f"Invalid model requested: {create_chat_completion_request.model}")
                    raise HTTPException(status_code=HttpStatusCodes.BAD_REQUEST, detail=error_message)
                logger.info(f"Model validation passed for: {create_chat_completion_request.model}")

            result = GenieWrapperCreateChatCompletion.create_chat_completion(create_chat_completion_request)

            if isinstance(result, Error):
                logger.error(f"Expected CreateChatCompletionResponse, got {type(result)}")
                raise HTTPException(status_code=int(result.code), detail=result.message)

            logger.info(f"create_chat_completion result: {result}")
            return result

        except HTTPException as http_exc:
            logger.error(f"HTTPException error in create_chat_completion: {http_exc}")
            raise HTTPException(status_code=http_exc.status_code, detail=http_exc.detail)

        except Exception as e:
            logger.error(f"Unexpected error in create_chat_completion: {e}")
            raise HTTPException(status_code=HttpStatusCodes.INTERNAL_SERVER_ERROR, detail=ErrorMessages.UNEXPECTED_ERROR)
