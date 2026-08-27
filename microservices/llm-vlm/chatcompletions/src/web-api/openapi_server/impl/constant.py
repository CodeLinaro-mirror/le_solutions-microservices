# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import os
from typing import Optional
# GENAI interface file path
# This path is mounted path of container. Check docker-compose volume section for path in host machine.
GENAI_INTERFACE_FILE = '/iot-user/app/site-packages/genai_interface.h'

# Sampler config file path
SAMPLER_CONFIG_PATH = '/iot-user/app/site-packages/sampler.json'

# DEPRECATED: This threshold is no longer used in the new summarization logic (SDK 2.45+)
# The new formula uses SUMMARIZATION_CONTEXT_THRESHOLD with system prompt overhead calculation.
# Kept for backward compatibility only.
SUMMARIZATION_THRESHOLD = float(os.getenv("GENAI_SUMMARIZATION_THRESHOLD", "0.7"))

# New Summarization Constants (SDK 2.45+)
# Context threshold: Trigger summarization when projected usage exceeds this percentage
SUMMARIZATION_CONTEXT_THRESHOLD = 0.9  # 90% of context size

# Summary size: Maximum tokens for generated summary as percentage of context size
SUMMARIZATION_SUMMARY_SIZE_RATIO = 0.2  # 20% of context size

# System prompt overhead multiplier: Accounts for formatting overhead in prompts
SUMMARIZATION_SYSTEM_PROMPT_OVERHEAD = 1.3  # 1.3x multiplier for system prompt tokens

# Max completion tokens multiplier: Reduces weight of max_completion_tokens
SUMMARIZATION_MAX_COMPLETION_MULTIPLIER = 0.5  # 50% weight for max_completion tokens

# ── Prompt slot ceilings (tokens) ─────────────────────────────────────────────
# Hard token ceilings for each named slot in the assembled prompt.
# These ensure no single slot can crowd out the history queue.
SLOT_SYSTEM_CEILING        = int(os.getenv("SLOT_SYSTEM_CEILING", "256"))
SLOT_TOOLS_CEILING         = int(os.getenv("SLOT_TOOLS_CEILING", "300"))
SLOT_FACTS_CEILING         = int(os.getenv("SLOT_FACTS_CEILING", "300"))
SLOT_SUMMARY_CEILING       = int(os.getenv("SLOT_SUMMARY_CEILING", "400"))

# ── Post-turn processing (eviction) ───────────────────────────────────────────
# Trigger eviction when history token usage exceeds this fraction of history budget
HISTORY_EVICTION_THRESHOLD = float(os.getenv("HISTORY_EVICTION_THRESHOLD", "0.85"))
# Target usage after eviction (fraction of history budget)
HISTORY_EVICTION_TARGET    = float(os.getenv("HISTORY_EVICTION_TARGET", "0.75"))

# ── Structured summarization prompt ───────────────────────────────────────────
STRUCTURED_SUMMARY_PROMPT = (
    "Summarize this conversation. Your summary will be used as context for future turns.\n"
    "Preserve:\n"
    "1. FACTS: Names, preferences, constraints, decisions\n"
    "2. TASK: What the user is trying to accomplish\n"
    "3. KEY EXCHANGES: Important questions asked and answers given\n"
    "4. OPEN ITEMS: Unresolved topics\n\n"
    "Write a compact paragraph. Maximum {max_tokens} tokens.\n\n"
    "Conversation:\n{conversation}\n\nSummary:"
)

# ── Fact extraction prompt ─────────────────────────────────────────────────────
# NOTE: {{}} is escaped braces — produces literal {} in the formatted string.
FACT_EXTRACTION_PROMPT = (
    "Extract persistent facts from this conversation. "
    "Return ONLY a JSON object with short string keys and values.\n"
    "Include: names, goals, decisions, constraints, preferences, current state.\n"
    "If a fact was established then contradicted, keep only the latest value.\n"
    "If no facts found, return {{}}.\n\n"
    "Conversation:\n{conversation}\n\nJSON:"
)

# --- Max completion tokens cap (context overflow prevention) ---
# Safety margin subtracted from the hard cap to absorb estimator error,
# BOS/EOS markers, and chat template overhead not explicitly tracked.
MAX_COMPLETION_SAFETY_MARGIN = 64

# Reject requests that would leave too little space for a meaningful answer.
MIN_USEFUL_COMPLETION_TOKENS = 64

# Token estimation buffer: fraction of prompt_tokens added to the cap calculation
# to account for the discrepancy between the character-based heuristic and the
# actual tokenizer. A 10% buffer means the service treats a 1000-token estimate
# as 1100 tokens, reducing the risk of silent truncation on dense content
# (code, JSON, non-English text) where the heuristic underestimates.
TOKEN_ESTIMATION_BUFFER_RATIO = float(os.getenv("TOKEN_ESTIMATION_BUFFER_RATIO", "0.10"))

# Default max completion tokens when the client does not specify max_completion_tokens
# or max_tokens. Used as the output reservation ceiling in both the pre-assembly query
# length guard (_check_user_query_length) and the prompt assembler (_build_complete_prompt_context).
# The assembler uses min(requested, DEFAULT_MAX_COMPLETION_TOKENS) so clients requesting
# fewer tokens get a proportionally larger input budget, while clients requesting more
# are capped at this value for budget accounting purposes.
# Override via DEFAULT_MAX_COMPLETION_TOKENS env variable.
DEFAULT_MAX_COMPLETION_TOKENS = int(os.getenv("DEFAULT_MAX_COMPLETION_TOKENS", "512"))

# Fraction of raw image token count to use for VLM budget estimation.
# The raw patch count (864 for a 512×342 image) may overestimate the actual tokens
# consumed by the LLM after preprocessing optimizations (patch merging, compression).
# Set below 1.0 to allow requests that would otherwise be incorrectly rejected.
# Default 0.7 means 864 raw patches → 604 estimated tokens per image.
IMAGE_TOKEN_ESTIMATION_RATIO = float(os.getenv("IMAGE_TOKEN_ESTIMATION_RATIO", "0.7"))

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
    REQUEST_TIMEOUT = 408

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
    DEFAULT_TOP_K = 40
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

    # We now dynamically determine this based on model context size (0.5x).
    # Leaving this constant as a fallback.
    DEFAULT_MAX_COMPLETION_TOKENS = 512

    CONTEXT_THRESHOLD_PERCENTAGE = 0.8  # 80% threshold for summarization

class ErrorMessages:
    """ Error Messages. """
    NOT_IMPELEMENTED = "Not implemented"
    UNEXPECTED_ERROR = "System resources are busy. Consider using a model with a smaller context size in your requests."
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
    CONTEXT_LENGTH_EXCEEDED = (
        "max_completion_tokens={requested} exceeds the available budget for this model. "
        "Reduce max_completion_tokens to {cap} or lower, or shorten your prompt. "
        "Model context window: {context_size}."
    )
    CONTEXT_LENGTH_EXCEEDED_TOOL_RESPONSE = (
        "The combined prompt and tool response exceed the available budget for this model. "
        "Reduce max_completion_tokens to {cap} or lower, shorten your prompt, or return a smaller tool response. "
        "Model context window: {context_size}."
    )
    PROMPT_TOO_LONG = (
        "Prompt consumes nearly the entire {context_size}-token context window. "
        "Only {cap} tokens remain for the response, which is below the minimum useful size. "
        "Please shorten your prompt."
    )
    PROMPT_TOO_LONG_TOOL_RESPONSE = (
        "The combined prompt and tool response consume nearly the entire {context_size}-token context window. "
        "Only {cap} tokens remain for the response, which is below the minimum useful size. "
        "Please shorten your prompt or return a smaller tool response."
    )
    USER_QUERY_TOO_LONG = (
        "The model has a {context_size}-token context window. "
        "Your message is approximately {query_tokens} tokens, but only {max_query_tokens} tokens "
        "are available for user input after reserving space for response output ({output_tokens} tokens) "
        "and other internal settings. "
        "Please shorten your message and retry."
    )
    VLM_QUERY_TOO_LONG = (
        "The model has a {context_size}-token context window. "
        "Your request is approximately {total_tokens} tokens "
        "(image: {image_tokens}, text: {text_tokens}), but only {available} tokens "
        "are available after reserving space for response output ({output_tokens} tokens) "
        "and other internal settings. "
        "Please use shorter text and retry."
    )

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
    DEFAULT_CONFIG_PATH = "/iot-user/app/openapi_server/configs/models_config.json"
    DEFAULT_CONFIGS_DIR = "/iot-user/app/openapi_server/configs"
    ENV_CONFIG_PATH_KEY = "GENAI_MODELS_CONFIG_PATH"

# ADHOC Mode Configuration
# When enabled, LLM handles are created and destroyed for each conversation turn
# This prevents QAIRT handle conflicts when multiple containers access the same NSP
ADHOC_MODE = os.getenv("ADHOC_MODE", "false").lower() in ("true", "1", "yes", "on")

try:
    TOOL_RESPONSE_TIMEOUT_SECONDS = max(1, int(os.getenv("TOOL_RESPONSE_TIMEOUT_SECONDS", "30")))
except (TypeError, ValueError):
    TOOL_RESPONSE_TIMEOUT_SECONDS = 30

class SystemResourceConstants:
    """
    System Resource Management Constants
    Configuration for memory guardrails and resource management.
    """
    # Memory headroom percentage to keep as buffer (default: 15%)
    MEMORY_HEADROOM_PERCENT = 15

    # Enable/disable resource guardrails (default: True)
    ENABLE_RESOURCE_GUARDRAILS = True

class GenieErrorMappings:
    """ Mapping of internal Genie SDK error codes to layman API response messages. """
    MAPPINGS = {
        "1002": "The system is not configured correctly to run AI models.",
        "1003": "This AI model cannot be run on your current hardware.",
        "5000": "This AI model is not compatible with the system's current software version.",
        "6000": "This AI model is not supported on this system.",
        "6001": "This AI model is not supported on this system.",
        "14001": "System resources are busy. Consider using a model with a smaller context size in your requests.",
        "14003": "The system encountered an issue while cleaning up memory."
    }

    SERVICE_UNAVAILABLE_CODES = {"1002","14001"}
    SERVICE_UNAVAILABLE_PATTERNS = (
        "system resources are busy",
        "insufficient system memory",
        "memory exhaustion",
        "out of memory",
        "temporarily unavailable",
        "cannot proceed even after evicting all idle processes",
    )

    @classmethod
    def extract_error_code(cls, error_string: str) -> Optional[str]:
        """Extract first known Genie SDK code found in error_string."""
        import re
        if not error_string:
            return None

        for code in cls.MAPPINGS:
            if re.search(rf"\b{code}\b", str(error_string)):
                return code
        return None

    @classmethod
    def is_service_unavailable_error(cls, error_string: str) -> bool:
        """
        Determine whether an error is transient/service-level pressure and
        should be surfaced as HTTP 503.
        """
        if not error_string:
            return False

        code = cls.extract_error_code(error_string)
        if code in cls.SERVICE_UNAVAILABLE_CODES:
            return True

        error_lower = str(error_string).lower()
        return any(pattern in error_lower for pattern in cls.SERVICE_UNAVAILABLE_PATTERNS)

    @classmethod
    def get_http_status_code(
        cls,
        error_string: str,
        default_status: int = HttpStatusCodes.INTERNAL_SERVER_ERROR,
    ) -> int:
        """Map error_string to an HTTP status while preserving non-resource defaults."""
        if cls.is_service_unavailable_error(error_string):
            return HttpStatusCodes.SERVICE_UNAVAILABLE
        return default_status

    @classmethod
    def get_layman_message(cls, error_string: str) -> str:
        """ Parses the error string for SDK error codes and returns the layman message. """
        import re
        if not error_string:
            return None

        for code, message in cls.MAPPINGS.items():
            if re.search(rf'\b{code}\b', str(error_string)):
                return message

        # Handle specific common string-based errors that don't have codes but indicate resource exhaustion
        if "NULL returned" in str(error_string) or "socket closed" in str(error_string).lower():
            return cls.MAPPINGS["14001"]

        # Handle broken pipe/connection reset errors (process crash)
        error_lower = str(error_string).lower()
        if "broken pipe" in error_lower or "connection reset" in error_lower or "errno 32" in error_lower or "errno 104" in error_lower:
            return "Service temporarily unavailable. Please try again."

        return None


# OpenAI-compatible error code for prompt/context overflows.
ERROR_CODE_CONTEXT_LENGTH_EXCEEDED = "context_length_exceeded"
