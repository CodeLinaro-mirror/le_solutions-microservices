# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import os
from openapi_server.impl.constant import LLMServiceQueryConstant as QUERY_CONST
from openapi_server.managers.model_config_manager import get_config_manager
from openapi_server.logger.logger_config import LoggerConfig

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)

class CommonUtils:
    @staticmethod
    def copy_py_string_to_c_array(ffi, c_array, py_string: str, max_length: int):
        """
        Safely copies a Python string into a fixed-size C char array using CFFI.

		Parameters:
		- ffi: The CFFI FFI instance.
		- c_array: The C char array field (e.g., struct.field).
		- py_string: The Python string to copy.
		- max_length: The total size of the C char array (including space for null terminator).
		"""
        encoded = py_string.encode('utf-8')[:max_length - 1]  # Reserve space for null terminator
        ffi.memmove(c_array, encoded, len(encoded))
        c_array[len(encoded)] = b'\0'  # Ensure null termination

    @staticmethod
    def copy_py_float_to_c_field(ffi, c_struct, field_name, py_float_value):
        """
        Safely copies a Python float value to a C struct float field using CFFI.

        Parameters:
        - ffi: The CFFI FFI instance.
        - c_struct: The C struct instance.
        - field_name: The name of the float field in the struct.
        - py_float_value: The Python float value to copy.
        """
        # Use ffi.addressof to get pointer to the field and assign
        setattr(c_struct, field_name, float(py_float_value))

    @staticmethod
    def copy_py_int_to_c_field(ffi, c_struct, field_name, py_int_value):
        """
        Safely copies a Python float value to a C struct float field using CFFI.

        Parameters:
        - ffi: The CFFI FFI instance.
        - c_struct: The C struct instance.
        - field_name: The name of the float field in the struct.
        - py_float_value: The Python float value to copy.
        """
        # Use ffi.addressof to get pointer to the field and assign
        setattr(c_struct, field_name, int(py_int_value))


    @staticmethod
    def get_model_name():
        """
        Get the model name to use for LLM initialization.
        Priority:
        1. GENAI_MODEL_NAME environment variable (for backward compatibility)
        2. Default model from configuration file
        3. Hardcoded fallback

        Returns:
            str: The external model ID (e.g., llama3-8b, qwen2.5-7b)
        """
        from openapi_server.managers.model_config_manager import ModelConfigManager
        from openapi_server.logger.logger_config import LoggerConfig

        LoggerConfig.initialize()
        logger = LoggerConfig.get_logger(__name__)

        # Check environment variable first (backward compatibility)
        env_model = os.getenv("GENAI_MODEL_NAME")
        if env_model:
            logger.info(f"Using model from GENAI_MODEL_NAME env var: {env_model}")
            return env_model

        # Get default model from configuration (returns external ID)
        try:
            config_manager = ModelConfigManager()
            default_model_id = config_manager.get_default_model()
            logger.info(f"Using default model from config: {default_model_id}")
            return default_model_id
        except Exception as e:
            logger.error(f"Error getting model from config: {e}, using fallback")
            return "qwen2.5-7b"  # External ID fallback

    @staticmethod
    def get_model_config_path(model_id: str) -> str:
        """
        Get the configuration file path for a given model.

        Args:
            model_id: The external model identifier (e.g., "qwen2.5-7b", "llama3-8b")

        Returns:
            str: Path to the model's Genie configuration file
        """
        from openapi_server.managers.model_config_manager import ModelConfigManager
        from openapi_server.logger.logger_config import LoggerConfig

        LoggerConfig.initialize()
        logger = LoggerConfig.get_logger(__name__)

        try:
            config_manager = ModelConfigManager()

            # Try as external model ID first
            config_path = config_manager.get_config_file_path(model_id)
            if config_path:
                logger.info(f"Found config path for model {model_id}: {config_path}")
                return config_path

            logger.warning(f"No config path found for model {model_id}")
            return "genie_config_llama3_1_8B.json"

        except Exception as e:
            logger.error(f"Error getting config path for model {model_id}: {e}")
            return "genie_config_llama3_1_8B.json"

    @staticmethod
    def get_chat_template(model_id: str) -> dict:
        """
        Get chat template from config with fallback support.

        Args:
            model_id: The model identifier

        Returns:
            Dict containing chat template components
        """
        config_manager = get_config_manager()

        # Try to get model-specific template
        chat_template = config_manager.get_chat_template(model_id)

        if chat_template:
            return chat_template

        # Fall back to global fallback_chat_template
        fallback = config_manager.models_config.get('fallback_chat_template')

        if fallback:
            logger.warning(f"No chat_template for model {model_id}, using fallback")
            return fallback

        # Ultimate fallback (should never reach here if config is valid)
        logger.error(f"No chat_template or fallback found for model {model_id}, using hardcoded default")
        return {
            'system_prefix': '<|im_start|>system\n',
            'system_suffix': '<|im_end|>\n',
            'user_prefix': '<|im_start|>user\n',
            'user_suffix': '<|im_end|>\n',
            'assistant_prefix': '<|im_start|>assistant\n',
            'assistant_suffix': '<|im_end|>\n',
            'default_system_prompt': 'You are a helpful assistant.'
        }

    @staticmethod
    def format_system_message(model_id: str, content: str) -> str:
        """Format system message using chat template."""
        template = CommonUtils.get_chat_template(model_id)
        return f"{template['system_prefix']}{content}{template['system_suffix']}"

    @staticmethod
    def format_user_message(model_id: str, content: str, has_vision: bool = False) -> str:
        """Format user message using chat template."""
        template = CommonUtils.get_chat_template(model_id)

        if has_vision and 'vision_start' in template and 'vision_end' in template:
            vision_tokens = f"{template['vision_start']}{template['vision_end']}"
            formatted_content = f"{vision_tokens} {content}"
        else:
            formatted_content = content

        return f"{template['user_prefix']}{formatted_content}{template['user_suffix']}"

    @staticmethod
    def format_assistant_message(model_id: str, content: str = None) -> str:
        """Format assistant message using chat template."""
        template = CommonUtils.get_chat_template(model_id)

        if content:
            # Use assistant_suffix if present, otherwise assume it's included or handled elsewhere
            suffix = template.get('assistant_suffix', '<|im_end|>\n') # Fallback for safety
            return f"{template['assistant_prefix']}{content}{suffix}"
        else:
            # Just the prefix (for prompting)
            return template['assistant_prefix']

    @staticmethod
    def build_chat_prompt(
        model_id: str,
        messages: list,
        include_assistant_prefix: bool = True,
        has_vision: bool = False,
        add_system_prompt: bool = True
    ) -> str:
        """
        Build complete chat prompt from messages using unified template.

        Args:
            model_id: Model identifier
            messages: List of message dicts with 'role' and 'content'
            include_assistant_prefix: Whether to add assistant prefix at end
            has_vision: Whether this is a vision request (adds vision tokens to last user message)
            add_system_prompt: Whether to prepend the system prompt (default True)

        Returns:
            Complete formatted prompt string
        """
        template = CommonUtils.get_chat_template(model_id)
        formatted_parts = []

        # Add global prefix if present in the template
        global_prefix = template.get('global_prefix')
        if global_prefix:
            formatted_parts.append(global_prefix)

        # Extract or use default system prompt
        system_prompt = template.get('default_system_prompt', 'You are a helpful assistant.')
        has_explicit_system = False

        for msg in messages:
            # Handle both dict and object (e.g. from Pydantic model)
            if isinstance(msg, dict):
                role = msg.get('role', '')
                content = msg.get('content', '')
            else:
                role = getattr(msg, 'role', '')
                content = getattr(msg, 'content', '')

            if role == 'system':
                system_prompt = content
                has_explicit_system = True
                break

        # Add system message if requested
        if add_system_prompt:
            formatted_parts.append(CommonUtils.format_system_message(model_id, system_prompt))

        # Track last user message index for vision token placement
        last_user_idx = -1
        processed_messages = []

        # Convert all to dicts first to simplify processing
        for msg in messages:
            if isinstance(msg, dict):
                processed_messages.append(msg)
            else:
                processed_messages.append({
                    'role': getattr(msg, 'role', ''),
                    'content': getattr(msg, 'content', ''),
                    'tool_calls': getattr(msg, 'tool_calls', None)
                })

        for i, msg in enumerate(processed_messages):
            if msg.get('role') == 'user':
                last_user_idx = i

        # Format remaining messages
        for i, msg in enumerate(processed_messages):
            role = msg.get('role', '')
            content = msg.get('content', '')

            if role == 'system':
                continue  # Already processed

            if role == 'tool':
                continue  # Skip tool messages (handled separately in tool calling logic, or should be?)
                # NOTE: In text_conversation_event.py, _execute_inference_with_tool specifically handles tool messages.
                # If we are using this builder there, we need to handle 'tool' role if it's passed.
                # For now, following the plan to skip, assuming tool handling logic builds prompt differently or injects into context.
                # Actually, wait. `_execute_inference_with_tool` constructs a prompt with tool results.
                # If we want this to be truly unified, we should probably handle 'tool' messages too if they are passed in.
                # But the standard chat completion flow separates tool handling.
                # Let's stick to the plan for now.

            if role == 'user':
                is_last_user = (i == last_user_idx)
                formatted_parts.append(
                    CommonUtils.format_user_message(model_id, content, has_vision and is_last_user)
                )
            elif role == 'assistant':
                tool_calls = msg.get('tool_calls')
                if tool_calls:
                    # Assistant message with tool calls (typically no content)
                    # Use suffix from template or fallback
                    suffix = template.get('assistant_suffix', '<|im_end|>\n')
                    formatted_parts.append(template['assistant_prefix'] + suffix)
                else:
                    # Regular assistant message
                    formatted_parts.append(CommonUtils.format_assistant_message(model_id, content))

        # Add assistant prefix for prompting
        if include_assistant_prefix:
            formatted_parts.append(template['assistant_prefix'])

        return ''.join(formatted_parts)
