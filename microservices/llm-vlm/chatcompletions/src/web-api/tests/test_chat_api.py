# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# coding: utf-8
import os
from fastapi.testclient import TestClient


from pydantic import Field, StrictStr  # noqa: F401
from typing_extensions import Annotated  # noqa: F401
from openapi_server.models.create_chat_completion_request import CreateChatCompletionRequest  # noqa: F401
from openapi_server.models.create_chat_completion_response import CreateChatCompletionResponse  # noqa: F401
from test_utils import TestUtils
from test_constant import ChatTestConstants as CHAT_CONST
from openapi_server.impl.constant import LLMServiceQueryConstant as TEST_CONST

def test_add_long_chat_completion(client: TestClient):
    """Test case for add_chat_completion

    Adds a long chat to the conversation.
    """
    req = CHAT_CONST.CREATE_CHAT_COMPLETION_REQUEST
    content = req[CHAT_CONST.KEY_MESSAGES][0][CHAT_CONST.KEY_CONTENT]
    old_content = content
    content = "a" * (TEST_CONST.MESSAGE_CONTENT_MAX_SIZE + 1)

    req[CHAT_CONST.KEY_MESSAGES][0][CHAT_CONST.KEY_CONTENT] = content
    # create a chat
    response = TestUtils.create_chat_utils(client, req)

    # assert the status code of the HTTP response
    assert response.status_code == 400

    req[CHAT_CONST.KEY_MESSAGES][0][CHAT_CONST.KEY_CONTENT] = old_content

def test_add_empty_chat_completion(client: TestClient):
    """Test case for add_chat_completion

    Adds a empty chat to the conversation.
    """
    req = CHAT_CONST.CREATE_CHAT_COMPLETION_REQUEST
    content = req[CHAT_CONST.KEY_MESSAGES][0][CHAT_CONST.KEY_CONTENT]
    old_content = content

    req[CHAT_CONST.KEY_MESSAGES][0][CHAT_CONST.KEY_CONTENT] = CHAT_CONST.EMPTY_MESSAGE
    # create a chat
    response = TestUtils.create_chat_utils(client, req)

    # assert the status code of the HTTP response
    assert response.status_code == 400

    req[CHAT_CONST.KEY_MESSAGES][0][CHAT_CONST.KEY_CONTENT] = old_content

def test_add_chat_completion(client: TestClient):
    """Test case for add_chat_completion

    Adds a chat to existing conversation.
    """
    # create a chat
    response = TestUtils.create_chat_utils(client, CHAT_CONST.CREATE_CHAT_COMPLETION_REQUEST)
    data = response.json()

    headers = {
    }
    # make add request
    response = client.request(
       "POST",
       "/v1/chat/completions/{completion_id}".format(completion_id=data[CHAT_CONST.KEY_RESPONSE_ID]),
       headers=headers,
       json=CHAT_CONST.CREATE_CHAT_COMPLETION_REQUEST,
    )

    # assert the status code of the HTTP response
    assert response.status_code == 200

    data = response.json()
    assert CHAT_CONST.KEY_CHOICES in data, "Expected {CHAT_CONST.KEY_CHOICES} to be present in the response"
    assert len(data[CHAT_CONST.KEY_CHOICES]), "Choices array is empty"

    choice = data[CHAT_CONST.KEY_CHOICES][0]
    assert CHAT_CONST.KEY_MESSAGE in choice, "Expected {CHAT_CONST.KEY_MESSAGE} to be present in the choice"

    message = choice[CHAT_CONST.KEY_MESSAGE]
    assert CHAT_CONST.KEY_CONTENT in message, "Expected {CHAT_CONST.KEY_CONTENT} to be present in the message"

    content = message[CHAT_CONST.KEY_CONTENT]
    assert content == CHAT_CONST.VAL_CONTENT, "Expected value for {CHAT_CONST.KEY_CONTENT} to be {CHAT_CONST.VAL_CONTENT}"

    # now delete the chat
    TestUtils.delete_chat_utils(client, data[CHAT_CONST.KEY_RESPONSE_ID])

def test_create_chat_completion(client: TestClient):
    """Test case for create_chat_completion

    Creates a chat conversation.
    """
    response = TestUtils.create_chat_utils(client, CHAT_CONST.CREATE_CHAT_COMPLETION_REQUEST)

    # assert the status code of the HTTP response
    assert response.status_code == 200

    data = response.json()
    assert CHAT_CONST.KEY_CHOICES in data, "Expected {CHAT_CONST.KEY_CHOICES} to be present in the response"
    assert len(data[CHAT_CONST.KEY_CHOICES]), "Choices array is empty"

    choice = data[CHAT_CONST.KEY_CHOICES][0]
    assert CHAT_CONST.KEY_MESSAGE in choice, "Expected {CHAT_CONST.KEY_MESSAGE} to be present in the choice"

    message = choice[CHAT_CONST.KEY_MESSAGE]
    assert CHAT_CONST.KEY_CONTENT in message, "Expected {CHAT_CONST.KEY_CONTENT} to be present in the message"

    content = message[CHAT_CONST.KEY_CONTENT]

    assert content == CHAT_CONST.VAL_CONTENT, "Expected value for {CHAT_CONST.KEY_CONTENT} to be {CHAT_CONST.VAL_CONTENT}"

    # now delete the chat
    TestUtils.delete_chat_utils(client, data[CHAT_CONST.KEY_RESPONSE_ID])

def test_delete_chat_completion(client: TestClient):
    """Test case for delete_chat_completion

    Deletes an existing conversation.
    """
    # create a chat
    response = TestUtils.create_chat_utils(client, CHAT_CONST.CREATE_CHAT_COMPLETION_REQUEST)
    data = response.json()

    # make a delete request
    response = TestUtils.delete_chat_utils(client, data[CHAT_CONST.KEY_RESPONSE_ID])

    # assert the status code of the HTTP response
    assert response.status_code == 200

    data = response.json()
    assert CHAT_CONST.KEY_OBJECT in data, "Expected {CHAT_CONST.KEY_OBJECT} to be present in the response"
    assert data[CHAT_CONST.KEY_OBJECT] == CHAT_CONST.VAL_OBJECT, "Expected value for {CHAT_CONST.KEY_OBJECT} is {CHAT_CONST.VAL_OBJECT}"

    assert CHAT_CONST.KEY_DELETED in data, "Expected {KEY_DELETED} to be present in the response"
    assert data[CHAT_CONST.KEY_DELETED] == CHAT_CONST.VAL_DELETED, "Expected value for {CHAT_CONST.KEY_DELETED} is {CHAT_CONST.VAL_DELETED}"

