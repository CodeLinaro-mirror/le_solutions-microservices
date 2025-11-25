# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# coding: utf-8

from typing import Dict, List  # noqa: F401
import importlib
import pkgutil

from openapi_server.apis.chat_api_base import BaseChatApi
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
from pydantic import Field, StrictStr
from typing_extensions import Annotated
from openapi_server.models.chat_completion_deleted import ChatCompletionDeleted
from openapi_server.models.create_chat_completion_request import CreateChatCompletionRequest
from openapi_server.models.create_chat_completion_response import CreateChatCompletionResponse
from openapi_server.impl.constant import (
    HttpStatusCodes,
    APIResponseKeys,
    APIDescription,
    ErrorMessages,
    APITags,
    APISummary
)

router = APIRouter()

ns_pkg = openapi_server.impl
for _, name, _ in pkgutil.iter_modules(ns_pkg.__path__, ns_pkg.__name__ + "."):
    importlib.import_module(name)

@router.post(
    "/v1/chat/completions",
    responses={
        HttpStatusCodes.OK: {APIResponseKeys.MODEL: CreateChatCompletionResponse, APIResponseKeys.DESCRIPTION: APIDescription.OK},
    },
    tags=[APITags.CHAT],
    summary=APISummary.CHAT_COMPLETION_CREATE,
    response_model_by_alias=True,
)
async def create_chat_completion(
    create_chat_completion_request: CreateChatCompletionRequest = Body(None, description=""),
) -> CreateChatCompletionResponse:
    if not BaseChatApi.subclasses:
        raise HTTPException(status_code=HttpStatusCodes.INTERNAL_SERVER_ERROR, detail=ErrorMessages.NOT_IMPELEMENTED)
    return await BaseChatApi.subclasses[0]().create_chat_completion(create_chat_completion_request)
