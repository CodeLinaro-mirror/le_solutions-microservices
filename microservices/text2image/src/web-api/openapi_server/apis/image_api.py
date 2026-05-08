# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# coding: utf-8

from typing import Dict, List  # noqa: F401
import importlib
import pkgutil

from openapi_server.apis.image_api_base import BaseImageApi
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
from openapi_server.models.image_generation_request import ImageGenerationRequest
from openapi_server.models.image_response import ImageResponse


router = APIRouter()

ns_pkg = openapi_server.impl
for _, name, _ in pkgutil.iter_modules(ns_pkg.__path__, ns_pkg.__name__ + "."):
    importlib.import_module(name)


@router.post(
    "/v1/images/generations",
    responses={
        200: {"model": ImageResponse, "description": "Image generated successfully"},
    },
    tags=["Image"],
    summary="Generate image from text prompt",
    response_model_by_alias=True,
)
async def v1_images_generations_post(
    image_generation_request: ImageGenerationRequest = Body(None, description=""),
) -> ImageResponse:
    """Generates images from scratch based on a text prompt using supported models."""
    if not BaseImageApi.subclasses:
        raise HTTPException(status_code=500, detail="Not implemented")
    return await BaseImageApi.subclasses[0]().v1_images_generations_post(image_generation_request)
