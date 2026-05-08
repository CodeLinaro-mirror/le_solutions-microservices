# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from fastapi.testclient import TestClient

class TestUtils:
    """
    Class for the utils module.
    This class contains the utility functions for the unit tests.
    """

    @staticmethod
    def create_chat_utils(client: TestClient, create_chat_completion_request: dict):
        """
        Creates a chat completion request for testing purposes.
        Args:
            create_chat_completion_request (dict): The chat completion request to create.
        Returns:
            dict: A chat completion response.
        """


        headers = {
        }
        # make a request
        response = client.request(
            "POST",
            "/v1/chat/completions",
            headers=headers,
            json=create_chat_completion_request,
        )
        return response

    @staticmethod
    def delete_chat_utils(client: TestClient, completion_id: str):
        """
        Deletes a chat completion request for testing purposes.
        Args:
            completion_id (str): The ID of the chat completion request to delete.
        """
        headers = {
        }
        # make a request
        response = client.request(
            "DELETE",
            f"/v1/chat/completions/{completion_id}",
            headers=headers,
        )
        return response


