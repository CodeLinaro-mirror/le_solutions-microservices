# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from openapi_server.apis.chat_api_base import BaseChatApi
from openapi_server.models.create_chat_completion_request import CreateChatCompletionRequest
from openapi_server.models.create_chat_completion_response import CreateChatCompletionResponse
from openapi_server.models.error import Error
from openapi_server.impl.event_based_chat_handler import EventBasedChatHandler
from openapi_server.logger.logger_config import LoggerConfig
from openapi_server.impl.constant import HttpStatusCodes, ErrorMessages
from openapi_server.managers.model_config_manager import ModelConfigManager

from fastapi import (
    HTTPException
)
from fastapi.responses import JSONResponse, StreamingResponse

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)

class ChatApiImpl(BaseChatApi):
    async def create_chat_completion(
    self,
    create_chat_completion_request: CreateChatCompletionRequest,
    raw_json: dict = None
    ):
        """
        Creates a new chat conversation.
        Args:
            create_chat_completion_request (CreateChatCompletionRequest): The chat completion to create.
            raw_json (dict): Raw JSON request body to bypass Pydantic deserialization issues.
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
                logger.debug(f"Model validation passed for: {create_chat_completion_request.model}")

            # FIX: Pass raw JSON to bypass Pydantic OneOf deserialization issues
            if raw_json:
                logger.debug("Received raw JSON dict to bypass Pydantic OneOf issues")
                logger.debug(f"Raw JSON contains {len(raw_json.get('messages', []))} messages")
            else:
                logger.debug("No raw JSON provided - may encounter Pydantic OneOf deserialization issues")

            # Use new event-based handler for conversation management
            logger.info("Using EventBasedChatHandler for request processing")
            result = await EventBasedChatHandler.handle_chat_completion(create_chat_completion_request, raw_json)

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
