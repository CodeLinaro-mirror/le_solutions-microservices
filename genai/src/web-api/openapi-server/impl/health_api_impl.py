# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from openapi_server.apis.health_api_base import BaseHealthApi
from typing import List
from openapi_server.models.usage_read import UsageRead
from datetime import datetime
from openapi_server.logger.logger_config import LoggerConfig
from openapi_server.impl.constant import HttpStatusCodes, ErrorMessages

from fastapi import (
    HTTPException
)

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)

class HealthApiImpl(BaseHealthApi):
    async def healthcheck(self) -> List[UsageRead]:
        """
        Health check endpoint.
        Args:
            None
        Returns:
            List[UsageRead]: List of UsageRead objects.
        Raises:
            HTTPException: If an unexpected error occurs.
        """
        try:
            result = [{
                "date_bin": datetime.now(),
                "total_generation_time": 1,
                "total_input_tokens": 1,
                "total_output_tokens": 1,
                "total_tokens": 1,
                "userid": "LLM_ON_9100"
            }]
            logger.info(f"add_chat_completion result: {result}")
            return result
        except Exception as e:
            logger.error(f"Unexpected error in get_health_check: {e}")
            raise HTTPException(status_code=HttpStatusCodes.INTERNAL_SERVER_ERROR, detail=ErrorMessages.UNEXPECTED_ERROR)
