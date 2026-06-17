# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from datetime import datetime
from fastapi import HTTPException

from openapi_server.apis.models_api_base import BaseModelsApi
from openapi_server.models.model_list_response import ModelListResponse
from openapi_server.models.model_list_response_data_inner import ModelListResponseDataInner
from openapi_server.logger.logger_config import LoggerConfig
from openapi_server.impl.constant import HttpStatusCodes, ErrorMessages
from openapi_server.managers.model_config_manager import model_config_manager

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)

class ModelsApiImpl(BaseModelsApi):
    async def list_models(self) -> ModelListResponse:
        """
        List available models endpoint.
        Returns:
            ModelListResponse: List of available models.
        """
        try:
            # Get models from configuration
            models = model_config_manager.get_models()

            if not models:
                logger.warning("No models configured in configuration file")
                raise HTTPException(
                    status_code=HttpStatusCodes.INTERNAL_SERVER_ERROR,
                    detail=ErrorMessages.NO_MODELS_CONFIGURED
                )

            # Create model entries from configuration
            model_list = []
            timestamp = int(datetime.now().timestamp())

            for model_id, model_info in models.items():
                model_entry = ModelListResponseDataInner(
                    id=model_id,
                    object="model",
                    created=timestamp,
                    owned_by=""
                )
                model_list.append(model_entry)

            # Create response with list of models
            response = ModelListResponse(
                object="list",
                data=model_list
            )

            logger.info(f"Returning list of {len(response.data)} model(s)")
            return response

        except FileNotFoundError as e:
            logger.error(f"Configuration file not found: {e}")
            raise HTTPException(
                status_code=HttpStatusCodes.INTERNAL_SERVER_ERROR,
                detail=ErrorMessages.CONFIG_FILE_NOT_FOUND
            )
        except ValueError as e:
            logger.error(f"Invalid configuration format: {e}")
            raise HTTPException(
                status_code=HttpStatusCodes.INTERNAL_SERVER_ERROR,
                detail=ErrorMessages.INVALID_CONFIG_FORMAT
            )
        except HTTPException:
            # Re-raise HTTP exceptions as-is
            raise
        except Exception as e:
            logger.error(f"Unexpected error in list_models: {e}")
            raise HTTPException(
                status_code=HttpStatusCodes.INTERNAL_SERVER_ERROR,
                detail=ErrorMessages.UNEXPECTED_ERROR
            )
