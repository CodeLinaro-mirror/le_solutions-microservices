# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from datetime import datetime
from typing import List

from openapi_server.apis.models_api_base import BaseModelsApi
from openapi_server.models.model_list_response import ModelListResponse
from openapi_server.models.model_list_response_data_inner import ModelListResponseDataInner
from openapi_server.logger.logger_config import LoggerConfig
from openapi_server.managers.model_config_manager import model_config_manager

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)

class ModelsApiImpl(BaseModelsApi):
    async def v1_models_get(self) -> ModelListResponse:
        """Return models from config file as per OpenAI schema"""

        try:
            # Get models from the config manager
            models = model_config_manager.get_models()
            
            models_data: List[ModelListResponseDataInner] = []
            
            # Iterate through models object where keys are model IDs
            for model_id, model_info in models.items():
                models_data.append(
                    ModelListResponseDataInner(
                        id=model_id,
                        object="model",
                        created=int(datetime.now().timestamp()),
                        owned_by=""
                    )
                )

            response = ModelListResponse(object="list", data=models_data)
            logger.debug(f"Models list response: {response}")
            return response
        except Exception as e:
            logger.error(f"Failed to get models list: {e}")
            raise
