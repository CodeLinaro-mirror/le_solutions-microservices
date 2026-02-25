# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# coding: utf-8

from typing import ClassVar, Tuple
from openapi_server.models.model import ModelListResponse

class BaseModelsApi:
    subclasses: ClassVar[Tuple] = ()

    def __init_subclass__(cls, **kwargs):
        super().__init_subclass__(**kwargs)
        BaseModelsApi.subclasses = BaseModelsApi.subclasses + (cls,)

    async def list_models(self) -> ModelListResponse:
        """Return a list of available models"""
        ...
