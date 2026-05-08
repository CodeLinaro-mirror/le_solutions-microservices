# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# coding: utf-8

from fastapi.testclient import TestClient


from openapi_server.models.create_completion_request import CreateCompletionRequest  # noqa: F401
from openapi_server.models.create_completion_response import CreateCompletionResponse  # noqa: F401
from test_constant import CompletionTestConstant as COMPLETION_CONST

def test_create_completion(client: TestClient):
    """Test case for create_completion

    Creates a completion for the provided prompt and parameters.
    """
    headers = {
    }
    # make a request
    response = client.request(
       "POST",
       "/v1/completions",
       headers=headers,
       json=COMPLETION_CONST.CREATE_COMPLETION_REQUEST,
    )

    #assert the status code of the HTTP response
    assert response.status_code == 200

    data = response.json()
    assert COMPLETION_CONST.KEY_CHOCIES in data, "Expected {COMPLETION_CONST.KEY_CHOCIES} to be present in the response"
    assert len(data[COMPLETION_CONST.KEY_CHOCIES]), "Choices array is empty"

    choice = data[COMPLETION_CONST.KEY_CHOCIES][0]
    assert COMPLETION_CONST.KEY_CONTENT in choice, "Expected {COMPLETION_CONST.KEY_CONTENT} to be present in the message"

    content = choice[COMPLETION_CONST.KEY_CONTENT]
    assert content == COMPLETION_CONST.VAL_CONTENT, "Expected value for {COMPLETION_CONST.KEY_CONTENT} to be {COMPLETION_CONST.VAL_CONTENT}"

