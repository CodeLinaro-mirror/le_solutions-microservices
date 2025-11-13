# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# coding: utf-8

from fastapi.testclient import TestClient


from openapi_server.models.image_generation_request import ImageGenerationRequest  # noqa: F401
from openapi_server.models.image_response import ImageResponse  # noqa: F401


def test_v1_images_generations_post(client: TestClient):
    """Test case for v1_images_generations_post

    Generate image from text prompt
    """
    image_generation_request = {"n":1,"quality":"auto","response_format":"url","output_format":"png","size":"512x512","output_compression":60,"stream":0,"background":"auto","moderation":"auto","partial_images":0,"model":"stable-diffusion-2.1","style":"vivid","prompt":"prompt","user":"user"}

    headers = {
    }
    # uncomment below to make a request
    #response = client.request(
    #    "POST",
    #    "/v1/images/generations",
    #    headers=headers,
    #    json=image_generation_request,
    #)

    # uncomment below to assert the status code of the HTTP response
    #assert response.status_code == 200

