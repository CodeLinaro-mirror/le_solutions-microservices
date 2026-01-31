# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import os

# GENAI interface file path
# This path is mounted path of container. Check docker-compose volume section for path in host machine.
GENAI_INTERFACE_FILE = '/root/app/site-packages/genai_interface.h'

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

class EnvVariableKeys:
    """ Constants for environment variable keys. """
    ENV_LIBRARY_PATH_KEY = "LLM_LIBRARY_PATH" # Key for the LLM library path environment variable
    ENV_VLM_LIBRARY_PATH_KEY = "VLM_LIBRARY_PATH" # Key for the VLM library path environment variable
    GENAI_INTERFACE_FILE_KEY = "GENAI_INTERFACE_FILE" # Key for the interface header env variable

class EnvVariableValues:
    """ Constants for environment variable values. """
    ENV_LIBRARY_PATH_DEFAULT_VAL = None # Will be computed dynamically if not set

class APIResponseKeys:
    """ API response keys. """
    MODEL = "model"
    DESCRIPTION = "description"
    # Chat Completion Response Keys
    CHAT_ID = "id"
    CHAT_OBJECT = "object"
    CHAT_CREATED = "created"
    CHAT_CHOICES = "choices"
    CHOICE_INDEX = "index"
    CHOICE_MESSAGE = "message"
    CHOICE_DELTA = "delta"
    CHOICE_FINISH_REASON = "finish_reason"

    # Message Keys
    MESSAGE_ROLE = "role"
    MESSAGE_CONTENT = "content"

class APIDescription:
    """ API description. """
    OK = "OK"
    SUCCESS = "Success"
    COMPELETION_ID = "id of conversation"
    DELETE_CHAT_COMPLETION_ID = "The ID of the chat completion to delete"
    DELETE_CHAT_SUCCESS = "The chat completion was deleted successfully"

class APITags:
    """ API Tags """
    CHAT = "Chat"
    COMPLETIONS = "Completions"
    HEALTH = "Health"
    PING = "Ping"
    MODELS = "Models"

class APISummary:
    """ API SUMMARY """
    CHAT_COMPLETION_WITH_ID = "Adds a chat to existing conversation. "
    CHAT_COMPLETION_CREATE = "Creates a chat conversation. "
    CHAT_COMPLETION_DELETE = "Delete a stored chat completion. "
    COMPLETION_CREATE = "Creates a completion for the provided prompt and parameters."
    HEALTH_CHECK = "Check the health of microservice"
    PING = "Checks if server is accessable to the client"
    MODELS_LIST = "Gets the Supported Models"

class LLMServiceQueryConstant:
    """ LLM service Query constants. All size in bytes. """
    ROLE_MAX_SIZE = 256
    MESSAGE_CONTENT_MAX_SIZE = 12300
    MODEL_STR_MAX_SIZE = 256
    DEFAULT_COMPLETION_TOKEN = 0
    DEFAULT_TEMPERATURE = 1.0
    DEFAULT_TOP_K = 0
    DEFAULT_TOP_P = 1.0
    DEFAULT_FREQUENCY_PENALTY = 0
    DEFAULT_PRESENCE_PENALTY = 0
    DEFAULT_STREAM = False
    DEFAULT_LOGPROBS = False
    DEFAULT_TOP_LOGPROBS = 0
    DEFAULT_N = 1
    DEFAULT_PARALLEL_TOOL_CALLS = False
    DEFAULT_ROLE = "user"
    DEFAULT_MODEL = "LLAMA3_1_8B"
    MAX_MESSAGE_COUNT = 12
    # Session and token management constants
    MAX_MESSAGE_PAIRS = 50  # Maximum conversation pairs (100 messages total)
    DEFAULT_MAX_COMPLETION_TOKENS = 1024  # Default if not provided by user
    CONTEXT_THRESHOLD_PERCENTAGE = 0.8  # 80% threshold for summarization

class ErrorMessages:
    """ Error Messages. """
    NOT_IMPELEMENTED = "Not implemented"
    UNEXPECTED_ERROR = "Unexpected error in processing request"
    INCORRECT_CONTENT = "Provide correct content"
    MAX_CONTENT_EXCEED = "Length of query exceeds the max length: " + str(LLMServiceQueryConstant.MESSAGE_CONTENT_MAX_SIZE)
    COMPLETION_ID_NOT_EXIST = "Completion id does not exist"
    OBJ_CREATION_FAILED = "LLM object creation failed"
    MEM_ALLOCATION_ERR = "Memory allocation error"
    CHAT_ID_EXISTS = "Converstaion already in progress with chat id: "
    MAX_MSG_LIMIT_REACHED = "You've hit the maximum message count for this chat. Start a new conversation to proceed."
    MODEL_NOT_FOUND = "Model '{model}' not found in configuration. Use GET /v1/models to see available models."
    MODEL_SWITCH_ERROR = "Error switching from model '{old_model}' to '{new_model}': {error}"
    MODEL_INIT_FAILED = "Failed to initialize model '{model}'. The model may be unavailable or incompatible with the current system."

class LLMServiceKeys:
    """ LLM Service Keys. """
    RESPONSE = "Response*"
    TOKEN = "mytoken"
    REFUSE = "refuse"
    QUERY = "Query*"
    RESPONSE_OBJ_TEXT_COMPLETION = "text_completion"
    FINISH_REASON_DEFAULT_VAL = "stop"
    CHAT_OBJECT_DEFAULT_VAL = "chat.completion"
    COMPLETION_OBJECT_DEFAULT_VAL = "chat.completion"

class Parameters:
    """ Parameters of API"""
    COMPLETION_ID = "completion_id"
    LLM_OBJECT = "llm_object"
    INTERNAL_TYPE = "internal"

class ModelConfigConstants:
    """ Model Configuration Constants """
    DEFAULT_CONFIG_PATH = "/root/app/openapi_server/configs/models_config.json"
    DEFAULT_CONFIGS_DIR = "/root/app/openapi_server/configs"
    ENV_CONFIG_PATH_KEY = "GENAI_MODELS_CONFIG_PATH"

# ADHOC Mode Configuration
# When enabled, LLM handles are created and destroyed for each conversation turn
# This prevents QAIRT handle conflicts when multiple containers access the same NSP
ADHOC_MODE = os.getenv("ADHOC_MODE", "false").lower() in ("true", "1", "yes", "on")
