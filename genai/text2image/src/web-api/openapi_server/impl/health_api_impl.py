# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from datetime import datetime
from typing import List
from fastapi import HTTPException

from openapi_server.apis.health_api_base import BaseHealthApi
from openapi_server.models.usage_read import UsageRead
from openapi_server.logger.logger_config import LoggerConfig
from openapi_server.impl.constant import HttpStatusCodes, ErrorMessages

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)

class HealthApiImpl(BaseHealthApi):
    async def healthcheck(self) -> List[UsageRead]:
        """
        Health check endpoint.
        Returns:
            List[UsageRead]: List of UsageRead objects.
        """
        try:
            result = [UsageRead(
                date_bin=datetime.now(),
                total_generation_time=1.0,
                total_input_tokens=1,
                total_output_tokens=1,
                total_tokens=1,
                userid="text2image_ON_9100"
            )]
            logger.info(f"Health check result: {result}")
            return result
        except Exception as e:
            logger.error(f"Unexpected error in healthcheck: {e}")
            raise HTTPException(status_code=HttpStatusCodes.INTERNAL_SERVER_ERROR,
                                detail=ErrorMessages.UNEXPECTED_ERROR)
