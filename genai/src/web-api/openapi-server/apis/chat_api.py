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
    Request,
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
from openapi_server.logger.logger_config import LoggerConfig
import logging

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)

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
    request: Request,
    create_chat_completion_request: CreateChatCompletionRequest = Body(None, description=""),
) -> CreateChatCompletionResponse:
    if not BaseChatApi.subclasses:
        raise HTTPException(status_code=HttpStatusCodes.INTERNAL_SERVER_ERROR, detail=ErrorMessages.NOT_IMPELEMENTED)

    # FIX: Get raw JSON body to bypass broken Pydantic OneOf deserialization
    # The Pydantic OneOf wrapper for multimodal content fails to deserialize properly,
    # so we extract the raw JSON before Pydantic processes it
    import json
    logger.debug("=== PYDANTIC ONEOF BYPASS FIX ===")
    logger.debug("Extracting raw JSON body to bypass broken Pydantic OneOf deserialization")

    raw_body = await request.body()
    raw_json = json.loads(raw_body.decode('utf-8')) if raw_body else {}

    if raw_json:
        messages_count = len(raw_json.get('messages', []))
        logger.debug(f"Successfully extracted raw JSON with {messages_count} messages")
        logger.debug(f"Raw JSON keys: {list(raw_json.keys())}")
    else:
        logger.debug("Raw JSON extraction resulted in empty dict")

    return await BaseChatApi.subclasses[0]().create_chat_completion(create_chat_completion_request, raw_json)
