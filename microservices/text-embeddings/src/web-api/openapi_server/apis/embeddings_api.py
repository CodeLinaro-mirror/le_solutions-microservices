# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# coding: utf-8

from typing import Dict, List  # noqa: F401
import importlib
import pkgutil

from openapi_server.apis.embeddings_api_base import BaseEmbeddingsApi
import openapi_server.impl

from fastapi import (  # noqa: F401
    APIRouter,
    Body,
    Cookie,
    Depends,
    Form,
    Header,
    HTTPException,
    Path,
    Query,
    Response,
    Security,
    status,
)

from openapi_server.models.extra_models import TokenModel  # noqa: F401
from typing import Any
from openapi_server.models.create_embeddings_request import CreateEmbeddingsRequest
from openapi_server.models.create_embeddings_response import CreateEmbeddingsResponse
from openapi_server.security_api import get_token_bearerAuth

router = APIRouter()

ns_pkg = openapi_server.impl
for _, name, _ in pkgutil.iter_modules(ns_pkg.__path__, ns_pkg.__name__ + "."):
    importlib.import_module(name)


@router.post(
    "/v1/embeddings",
    responses={
        200: {"model": CreateEmbeddingsResponse, "description": "Embeddings created successfully"},
        400: {"description": "Bad request"},
        401: {"description": "Unauthorized"},
    },
    tags=["Embeddings"],
    summary="Create embeddings",
    response_model_by_alias=True,
)
async def create_embeddings(
    create_embeddings_request: CreateEmbeddingsRequest = Body(None, description=""),
) -> CreateEmbeddingsResponse:
    """Creates an embedding vector representing the input text."""
    if not BaseEmbeddingsApi.subclasses:
        raise HTTPException(status_code=500, detail="Not implemented")
    return await BaseEmbeddingsApi.subclasses[0]().create_embeddings(create_embeddings_request)
