# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# coding: utf-8

from typing import ClassVar, Dict, List, Tuple  # noqa: F401

from openapi_server.models.image_generation_request import ImageGenerationRequest
from openapi_server.models.image_response import ImageResponse


class BaseImageApi:
    subclasses: ClassVar[Tuple] = ()

    def __init_subclass__(cls, **kwargs):
        super().__init_subclass__(**kwargs)
        BaseImageApi.subclasses = BaseImageApi.subclasses + (cls,)
    async def v1_images_generations_post(
        self,
        image_generation_request: ImageGenerationRequest,
    ) -> ImageResponse:
        """Generates images from scratch based on a text prompt using supported models."""
        ...
