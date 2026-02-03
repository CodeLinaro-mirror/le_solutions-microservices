# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# coding: utf-8

from typing import Dict, List  # noqa: F401
import importlib
import pkgutil

from openapi_server.apis.ping_api_base import BasePingApi
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
from openapi_server.models.ping_response import PingResponse
from openapi_server.security_api import get_token_bearerAuth

router = APIRouter()

ns_pkg = openapi_server.impl
for _, name, _ in pkgutil.iter_modules(ns_pkg.__path__, ns_pkg.__name__ + "."):
    importlib.import_module(name)


@router.get(
    "/v1/ping",
    responses={
        200: {"model": PingResponse, "description": "Success"},
    },
    tags=["Ping"],
    summary="Check if server is accessible to the client",
    response_model_by_alias=True,
)
async def ping() -> PingResponse:
    """Ping the server to check it&#39;s alive and well. Returns &#39;Pong&#39; if the server is up."""
    if not BasePingApi.subclasses:
        raise HTTPException(status_code=500, detail="Not implemented")
    return await BasePingApi.subclasses[0]().ping()
