# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# coding: utf-8

from typing import ClassVar, Dict, List, Tuple  # noqa: F401

from typing import Any
from openapi_server.models.create_embeddings_request import CreateEmbeddingsRequest
from openapi_server.models.create_embeddings_response import CreateEmbeddingsResponse
from openapi_server.security_api import get_token_bearerAuth

class BaseEmbeddingsApi:
    subclasses: ClassVar[Tuple] = ()

    def __init_subclass__(cls, **kwargs):
        super().__init_subclass__(**kwargs)
        BaseEmbeddingsApi.subclasses = BaseEmbeddingsApi.subclasses + (cls,)
    async def create_embeddings(
        self,
        create_embeddings_request: CreateEmbeddingsRequest,
    ) -> CreateEmbeddingsResponse:
        """Creates an embedding vector representing the input text."""
        ...
