# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# coding: utf-8

from openapi_server.apis.models_api_base import BaseModelsApi
from openapi_server.models.model import Model, ModelListResponse
from openapi_server.managers.model_config_manager import ModelConfigManager
from openapi_server.logger.logger_config import LoggerConfig
import time

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)


class ModelsApiImpl(BaseModelsApi):
    async def list_models(self) -> ModelListResponse:
        """
        List all available models from the configuration file.

        Returns:
            ModelListResponse: List of available models in OpenAI-compatible format
        """
        try:
            config_manager = ModelConfigManager()
            available_models = config_manager.get_available_models()

            now = int(time.time())
            models = []

            for model_info in available_models:
                models.append(
                    Model(
                        id=model_info["id"],
                        created=now,
                        object="model",
                        owned_by=model_info.get("owned_by", "")
                    )
                )

            logger.info(f"Returning {len(models)} available models")
            return ModelListResponse(data=models)

        except Exception as e:
            logger.error(f"Error listing models: {e}")
            # Fallback to default models if config fails
            now = int(time.time())
            models = [
                Model(id="qwen2-7b", created=now, object="model", owned_by=""),
            ]
            return ModelListResponse(data=models)
