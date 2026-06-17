# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# coding: utf-8

from typing import Dict, List  # noqa: F401
import importlib
import pkgutil

from openapi_server.apis.health_api_base import BaseHealthApi
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
from typing import List
from openapi_server.models.usage_read import UsageRead
from openapi_server.security_api import get_token_bearerAuth

router = APIRouter()

ns_pkg = openapi_server.impl
for _, name, _ in pkgutil.iter_modules(ns_pkg.__path__, ns_pkg.__name__ + "."):
    importlib.import_module(name)


@router.get(
    "/v1/health",
    responses={
        200: {"model": List[UsageRead], "description": "Success"},
    },
    tags=["Health"],
    summary="Check the health of microservice",
    response_model_by_alias=True,
)
async def healthcheck() -> List[UsageRead]:
    """Health check of various dependencies."""
    if not BaseHealthApi.subclasses:
        raise HTTPException(status_code=500, detail="Not implemented")
    return await BaseHealthApi.subclasses[0]().healthcheck()
