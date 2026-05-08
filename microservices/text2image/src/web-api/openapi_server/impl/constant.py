# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

class HttpStatusCodes:
    """
    Constants for HTTP status codes.
    """
    # Successful responses
    OK = 200
    CREATED = 201
    ACCEPTED = 202

    # Client error responses
    BAD_REQUEST = 400
    UNAUTHORIZED = 401
    FORBIDDEN = 403
    NOT_FOUND = 404

    # Server error responses
    INTERNAL_SERVER_ERROR = 500
    NOT_IMPLEMENTED = 501
    BAD_GATEWAY = 502
    SERVICE_UNAVAILABLE = 503
    GATEWAY_TIMEOUT = 504
    HTTP_VERSION_NOT_SUPPORTED = 505
    CONFLICT = 409

class ErrorMessages:
    """ Error Messages. """
    NOT_IMPELEMENTED = "Not implemented"
    UNEXPECTED_ERROR = "Unexpected error in processing request"
    INCORRECT_CONTENT = "Provide correct content"
    MAX_CONTENT_EXCEED = "Length of query exceeds the max length: "
    COMPLETION_ID_NOT_EXIST = "Completion id does not exist"
    OBJ_CREATION_FAILED = "text2image object creation failed"
    MEM_ALLOCATION_ERR = "Memory allocation error"
    CHAT_ID_EXISTS = "Converstaion already in progress with chat id: "
    MAX_MSG_LIMIT_REACHED = "You've hit the maximum message count for this chat. Start a new conversation to proceed."
    INVALID_PROMPT = "Prompt must contain meaningful text. Please provide a description with alphabetic characters."
