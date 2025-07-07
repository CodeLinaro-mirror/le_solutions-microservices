# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# coding: utf-8

from fastapi.testclient import TestClient


from typing import List  # noqa: F401
from openapi_server.models.usage_read import UsageRead  # noqa: F401


def test_healthcheck(client: TestClient):
    """Test case for healthcheck

    Check the health of microservice
    """

    headers = {
    }
    # make a request
    response = client.request(
       "GET",
       "/v1/health",
       headers=headers,
    )

    # assert the status code of the HTTP response
    assert response.status_code == 200

