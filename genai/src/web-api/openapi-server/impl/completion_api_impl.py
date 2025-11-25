# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from openapi_server.apis.completions_api_base import BaseCompletionsApi
from openapi_server.models.create_completion_request import CreateCompletionRequest
from openapi_server.models.create_completion_response import CreateCompletionResponse
from openapi_server.impl.genie_wrapper.completions.genie_wrapper_completion import GenieWrapperCreateCompletion
from openapi_server.logger.logger_config import LoggerConfig
from openapi_server.impl.constant import HttpStatusCodes, ErrorMessages
from openapi_server.models.error import Error

from fastapi import (
    HTTPException
)

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)

class CompletionApiImpl(BaseCompletionsApi):
    async def create_completion(
        self,
        create_completion_request: CreateCompletionRequest
    ) -> CreateCompletionResponse:
        """Creates a completion for the provided prompt and parameters."""
        try:
            result = GenieWrapperCreateCompletion.create_completion(create_completion_request)
            if isinstance(result, Error):
                logger.error(f"Expected CreateCompletionResponse, got {type(result)}")
                raise HTTPException(status_code=int(result.code), detail=result.message)
            else:
                logger.info(f"create_completion result: {result}")
                return result

        except HTTPException as http_exc:
                logger.error(f"HTTPException error in create_chat_completion: {http_exc}")
                raise HTTPException(status_code=http_exc.status_code, detail=http_exc.detail)
        except Exception as e:
            logger.error(f"Unexpected error in create_completion: {e}")
            raise HTTPException(status_code=HttpStatusCodes.INTERNAL_SERVER_ERROR, detail=ErrorMessages.UNEXPECTED_ERROR)
