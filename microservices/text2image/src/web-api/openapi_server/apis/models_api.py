# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# coding: utf-8

from typing import Dict, List  # noqa: F401
import importlib
import pkgutil

from openapi_server.apis.models_api_base import BaseModelsApi
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
from openapi_server.models.model_list_response import ModelListResponse


router = APIRouter()

ns_pkg = openapi_server.impl
for _, name, _ in pkgutil.iter_modules(ns_pkg.__path__, ns_pkg.__name__ + "."):
    importlib.import_module(name)


@router.get(
    "/v1/models",
    responses={
        200: {"model": ModelListResponse, "description": "A list of available models"},
    },
    tags=["Models"],
    summary="List available models",
    response_model_by_alias=True,
)
async def v1_models_get(
) -> ModelListResponse:
    """Returns a list of available models for image generation."""
    if not BaseModelsApi.subclasses:
        raise HTTPException(status_code=500, detail="Not implemented")
    return await BaseModelsApi.subclasses[0]().v1_models_get()
