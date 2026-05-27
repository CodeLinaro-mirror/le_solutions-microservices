# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# coding: utf-8

from fastapi.testclient import TestClient


from openapi_server.models.model_list_response import ModelListResponse  # noqa: F401


def test_v1_models_get(client: TestClient):
    """Test case for v1_models_get

    List available models
    """

    headers = {
    }
    # uncomment below to make a request
    #response = client.request(
    #    "GET",
    #    "/v1/models",
    #    headers=headers,
    #)

    # uncomment below to assert the status code of the HTTP response
    #assert response.status_code == 200

