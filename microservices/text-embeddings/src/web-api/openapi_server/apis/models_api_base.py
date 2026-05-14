# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# coding: utf-8

from typing import ClassVar, Dict, List, Tuple  # noqa: F401

from openapi_server.models.model_list_response import ModelListResponse
from openapi_server.security_api import get_token_bearerAuth

class BaseModelsApi:
    subclasses: ClassVar[Tuple] = ()

    def __init_subclass__(cls, **kwargs):
        super().__init_subclass__(**kwargs)
        BaseModelsApi.subclasses = BaseModelsApi.subclasses + (cls,)
    async def list_models(
        self,
    ) -> ModelListResponse:
        """Returns a list of available models."""
        ...
