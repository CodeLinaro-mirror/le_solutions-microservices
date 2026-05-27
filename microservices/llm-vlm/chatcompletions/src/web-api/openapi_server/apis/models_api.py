# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# coding: utf-8

import importlib
import pkgutil
import openapi_server.impl
from fastapi import APIRouter, HTTPException
from openapi_server.apis.models_api_base import BaseModelsApi
from openapi_server.models.model import ModelListResponse
from openapi_server.impl.constant import (
    HttpStatusCodes,
    APIResponseKeys,
    APIDescription,
    APITags,
    APISummary,
)

router = APIRouter()

# Dynamically import all modules under openapi_server.impl
ns_pkg = openapi_server.impl
for _, name, _ in pkgutil.iter_modules(ns_pkg.__path__, ns_pkg.__name__ + "."):
    importlib.import_module(name)

@router.get(
    "/v1/models",
    responses={
        HttpStatusCodes.OK: {
            APIResponseKeys.MODEL: ModelListResponse,
            APIResponseKeys.DESCRIPTION: APIDescription.SUCCESS,
        },
    },
    tags=[APITags.MODELS],
    summary=APISummary.MODELS_LIST,
    response_model_by_alias=True,
)
async def list_models() -> ModelListResponse:
    if not BaseModelsApi.subclasses:
        raise HTTPException(status_code=HttpStatusCodes.INTERNAL_SERVER_ERROR, detail="Not implemented")
    return await BaseModelsApi.subclasses[0]().list_models()
