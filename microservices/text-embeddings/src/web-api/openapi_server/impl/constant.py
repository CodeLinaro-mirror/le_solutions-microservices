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
    OBJ_CREATION_FAILED = "text2image object creation failed"
    MEM_ALLOCATION_ERR = "Memory allocation error"
    MAX_MSG_LIMIT_REACHED = "You've hit the maximum message count for this chat. Start a new conversation to proceed."
    CONFIG_FILE_NOT_FOUND = "Models configuration file not found"
    INVALID_CONFIG_FORMAT = "Invalid models configuration format"
    NO_MODELS_CONFIGURED = "No models configured in configuration file"
    MODEL_NOT_SUPPORTED = "Requested model is not supported"
