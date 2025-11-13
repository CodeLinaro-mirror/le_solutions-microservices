# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from openapi_server.apis.ping_api_base import BasePingApi
from openapi_server.models.ping_response import PingResponse
from openapi_server.logger.logger_config import LoggerConfig
from fastapi import  HTTPException
from openapi_server.impl.constant import HttpStatusCodes, ErrorMessages

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)

class PingApiImpl(BasePingApi):
    async def ping(self) -> PingResponse:
        """
        Ping endpoint that returns a simple response to verify service is running.
        Args:
            None
        returns:
            PingResponse: The response object.
        raises:
            HTTPException: If an unexpected error occurs.
        """
        try:
            response = PingResponse(message="Pong")
            return response
        except Exception as e:
            logger.error(f"Unexpected error in get_ping: {e}")
            raise HTTPException(status_code=HttpStatusCodes.INTERNAL_SERVER_ERROR, detail=ErrorMessages.UNEXPECTED_ERROR)

