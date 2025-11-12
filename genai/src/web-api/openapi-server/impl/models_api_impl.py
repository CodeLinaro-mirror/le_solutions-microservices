# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# coding: utf-8

from openapi_server.apis.models_api_base import BaseModelsApi
from openapi_server.models.model import Model, ModelListResponse
import time

class ModelsApiImpl(BaseModelsApi):
    async def list_models(self) -> ModelListResponse:
        now = int(time.time())
        models = [
            Model(id="LLAMA3_1_8B", created=now, object="model", owned_by=""),
            Model(id="QWEN2_5_7B", created=now, object="model", owned_by=""),
            Model(id="LLAMA3_2_3B", created=now, object="model", owned_by=""),
        ]

        return ModelListResponse(data=models)
