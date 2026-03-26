# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# coding: utf-8

from typing import Dict, List  # noqa: F401
import importlib
import pkgutil

from openapi_server.apis.health_api_base import BaseHealthApi
import openapi_server.impl
from openapi_server.impl.constant import (
    HttpStatusCodes,
    APIResponseKeys,
    APIDescription,
    ErrorMessages,
    APITags,
    APISummary
)

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
from typing import Dict


router = APIRouter()

ns_pkg = openapi_server.impl
for _, name, _ in pkgutil.iter_modules(ns_pkg.__path__, ns_pkg.__name__ + "."):
    importlib.import_module(name)


@router.get(
    "/v1/health",
    responses={
        HttpStatusCodes.OK: {APIResponseKeys.DESCRIPTION: "Service is healthy"},
        HttpStatusCodes.SERVICE_UNAVAILABLE: {APIResponseKeys.DESCRIPTION: "Service is unhealthy"},
    },
    tags=[APITags.HEALTH],
    summary=APISummary.HEALTH_CHECK,
    response_model_by_alias=True,
)
async def healthcheck():
    """
    Health check for container orchestration (Docker Swarm / Kubernetes).

    Returns HTTP 200 when healthy, HTTP 503 when unhealthy.
    Unhealthy conditions:
    - Any model has >= 3 consecutive inference failures (DSP/FastRPC fault)
    - Available system memory < 100 MB
    """
    if not BaseHealthApi.subclasses:
        raise HTTPException(status_code=HttpStatusCodes.INTERNAL_SERVER_ERROR, detail=ErrorMessages.NOT_IMPELEMENTED)
    return await BaseHealthApi.subclasses[0]().healthcheck()
