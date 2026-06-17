# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# coding: utf-8

from fastapi.testclient import TestClient


from typing import Any  # noqa: F401
from openapi_server.models.create_embeddings_request import CreateEmbeddingsRequest  # noqa: F401
from openapi_server.models.create_embeddings_response import CreateEmbeddingsResponse  # noqa: F401


def test_create_embeddings(client: TestClient):
    """Test case for create_embeddings

    Create embeddings
    """
    create_embeddings_request = {"input":"CreateEmbeddingsRequest_input","encoding_format":"float","model":"text-embedding-3-small","user":"user","dimensions":1}

    headers = {
        "Authorization": "Bearer special-key",
    }
    # uncomment below to make a request
    #response = client.request(
    #    "POST",
    #    "/v1/embeddings",
    #    headers=headers,
    #    json=create_embeddings_request,
    #)

    # uncomment below to assert the status code of the HTTP response
    #assert response.status_code == 200

