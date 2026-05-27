# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# coding: utf-8

from fastapi.testclient import TestClient


from openapi_server.models.ping_response import PingResponse  # noqa: F401


def test_ping(client: TestClient):
    """Test case for ping

    Check if server is accessible to the client
    """

    headers = {
    }
    # uncomment below to make a request
    #response = client.request(
    #    "GET",
    #    "/v1/ping",
    #    headers=headers,
    #)

    # uncomment below to assert the status code of the HTTP response
    #assert response.status_code == 200

