# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# coding: utf-8

from fastapi.testclient import TestClient


from openapi_server.models.ping_response import PingResponse  # noqa: F401
from test_constant import PingTestConstant as PING_CONST

def test_ping(client: TestClient):
    """Test case for ping

    Check if server is accessable to the client
    """

    headers = {
    }

    response = client.request(
       "GET",
       "/v1/ping",
       headers=headers,
    )

    assert response.status_code == 200
    data = response.json()
    assert PING_CONST.KEY_MSG in data, "Expected {PING_CONST.KEY_MSG} to be present in the response"
    assert data[PING_CONST.KEY_MSG] == PING_CONST.KEY_MSG_VAL, "Expected {PING_CONST.KEY_MSG} to be {PING_CONST.KEY_MSG_VAL}"

