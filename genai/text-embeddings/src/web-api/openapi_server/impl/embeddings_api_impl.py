# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import os
from typing import List
from fastapi import HTTPException

from openapi_server.apis.embeddings_api_base import BaseEmbeddingsApi
from openapi_server.models.create_embeddings_request import CreateEmbeddingsRequest
from openapi_server.models.create_embeddings_response import CreateEmbeddingsResponse
from openapi_server.models.embedding import Embedding
from openapi_server.models.embedding_embedding import EmbeddingEmbedding
from openapi_server.models.embedding_usage import EmbeddingUsage
from openapi_server.logger.logger_config import LoggerConfig
from openapi_server.impl.constant import HttpStatusCodes, ErrorMessages
from openapi_server.managers.model_config_manager import model_config_manager

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)

class EmbeddingsApiImpl(BaseEmbeddingsApi):
    async def create_embeddings(
        self,
        create_embeddings_request: CreateEmbeddingsRequest,
    ) -> CreateEmbeddingsResponse:
        """
        Create embeddings endpoint.

        Args:
            create_embeddings_request: Request containing model, input text, and optional parameters

        Returns:
            CreateEmbeddingsResponse: Response with embeddings data
        """
        try:
            # Validate model is supported
            requested_model = create_embeddings_request.model

            if not model_config_manager.is_model_available(requested_model):
                available_models = model_config_manager.get_available_model_ids()
                logger.error(f"Unsupported model requested: {requested_model}")
                raise HTTPException(
                    status_code=HttpStatusCodes.BAD_REQUEST,
                    detail=f"Model '{requested_model}' is not supported. Available models: {', '.join(available_models)}"
                )

            logger.info(f"Creating embeddings for model: {requested_model}")

            # Extract input - handle different input types (string, array of strings, array of token arrays)
            input_data = create_embeddings_request.input

            # Convert input to list of strings for processing
            if isinstance(input_data.actual_instance, str):
                inputs = [input_data.actual_instance]
            elif isinstance(input_data.actual_instance, list):
                inputs = input_data.actual_instance
            else:
                inputs = [str(input_data.actual_instance)]

            logger.info(f"Processing {len(inputs)} input(s)")

            # Resolve model binary path from config + MODELS_PATH env variable
            model_info = model_config_manager.get_model_info(requested_model)
            model_file = model_info["model_file"]
            models_path = os.environ.get("MODELS_PATH", "/opt/embed_gen/")
            model_binary_path = os.path.join(models_path, model_file)

            embeddings_data = await self._get_embeddings_from_component(
                inputs=inputs,
                model=model_binary_path,
                dimensions=create_embeddings_request.dimensions,
                encoding_format=create_embeddings_request.encoding_format
            )

            # Create embedding objects
            embedding_objects = []
            for idx, emb_vector in enumerate(embeddings_data):
                embedding_obj = Embedding(
                    object="embedding",
                    embedding=EmbeddingEmbedding(emb_vector),
                    index=idx
                )
                embedding_objects.append(embedding_obj)

            # Calculate token usage (dummy values for now)
            total_tokens = sum(len(str(inp).split()) for inp in inputs)
            usage = EmbeddingUsage(
                prompt_tokens=total_tokens,
                total_tokens=total_tokens
            )

            # Create response
            response = CreateEmbeddingsResponse(
                object="list",
                data=embedding_objects,
                model=create_embeddings_request.model,
                usage=usage
            )

            logger.info(f"Successfully created {len(embedding_objects)} embedding(s)")
            return response

        except HTTPException:
            raise
        except Exception as e:
            logger.error(f"Unexpected error in create_embeddings: {e}")
            raise HTTPException(
                status_code=HttpStatusCodes.INTERNAL_SERVER_ERROR,
                detail=ErrorMessages.UNEXPECTED_ERROR
            )

    async def _get_embeddings_from_component(
        self,
        inputs: List[str],
        model: str,
        dimensions: int = None,
        encoding_format: str = "float"
    ) -> List[List[float]]:
        """
        TODO: Replace this method with actual call to embeddings component.

        This is a placeholder that returns dummy embeddings.
        When the actual embeddings component is ready, replace this method
        to make HTTP/gRPC calls to that component.

        Args:
            inputs: List of input texts to embed
            model: Model name to use for embeddings
            dimensions: Optional dimension size for embeddings
            encoding_format: Format for embeddings (float or base64)

        Returns:
            List of embedding vectors (each vector is a list of floats)
        """
        logger.warning("Using dummy embeddings - replace with actual component call")

        # Return dummy embeddings (768-dimensional vectors of zeros)
        # Typical embedding dimensions: 768 (BERT), 1536 (OpenAI), etc.
        embedding_dim = dimensions if dimensions else 768

        dummy_embeddings = []
        for _ in inputs:
            # Create a dummy embedding vector
            dummy_vector = [0.0] * embedding_dim
            dummy_embeddings.append(dummy_vector)

        return dummy_embeddings
