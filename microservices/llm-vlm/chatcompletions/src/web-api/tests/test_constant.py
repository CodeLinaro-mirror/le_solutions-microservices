# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

class ChatTestConstants:
    # String keys
    KEY_CHOICES = "choices"
    KEY_MESSAGE = "message"
    KEY_CONTENT = "content"
    VAL_CONTENT = "The capital of India is New Delhi"
    KEY_OBJECT = "object"
    VAL_OBJECT = "chat.completion.deleted"
    KEY_DELETED = "deleted"
    VAL_DELETED = True
    KEY_RESPONSE_ID = "id"
    KEY_MESSAGES = "messages"
    EMPTY_MESSAGE = ""

    # JSON request definition
    CREATE_CHAT_COMPLETION_REQUEST = {
        "messages": [
            {
                "role": "user",
                "name": "name",
                "content": "what is the capital of India?"
            }
        ]
    }

class CompletionTestConstant:
    # String keys
    KEY_CHOCIES = "choices"
    KEY_CONTENT = "text"
    VAL_CONTENT = "The capital of India is New Delhi"

    CREATE_COMPLETION_REQUEST = {"prompt":"What is the capital of india?"}

class PingTestConstant:
    KEY_MSG = "message"
    KEY_MSG_VAL = "Pong"

class HandleObjectInterfaceConstant:
    VALUE_1 = "value1"
    VALUE_2 = "value2"
    ID_1 = "id1"
    ID_2 = "id2"
    ID_0 = "id0"
    MAX_ELEMENT_COUNT_FOUR = 4
    MAX_ELEMENT_COUNT_FIFTY = 50
    CURRENT_SIZE_COUNT_TWO = 2
    CURRENT_SIZE_COUNT_ZERO = 0
    MAPPING_LEN_TWO = 2
    MODEL = "my_model"
