# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import os
from openapi_server.impl.constant import LLMServiceQueryConstant as QUERY_CONST
from openapi_server.impl.model_config_manager import get_config_manager
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
        from openapi_server.impl.model_config_manager import ModelConfigManager
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
                     Also supports internal IDs for backward compatibility

        Returns:
            str: Path to the model's Genie configuration file
        """
        from openapi_server.impl.model_config_manager import ModelConfigManager
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

            # Try to find by internal ID
            external_id = config_manager.get_model_by_internal_id(model_id)
            if external_id:
                config_path = config_manager.get_config_file_path(external_id)
                if config_path:
                    logger.info(f"Found config path for internal model {model_id}: {config_path}")
                    return config_path

            # Fallback to hardcoded mapping
            logger.warning(f"No config path found for model {model_id}, using hardcoded fallback")
            fallback_map = {
                "LLAMA3_1_8B": "genie_config_llama3_1_8B.json",
                "LLAMA3_2_3B": "genie_config_llama3_2_3B.json",
                "QWEN2_5_7B": "genie_config_qwen2_5_7B.json"
            }
            return fallback_map.get(model_id, "genie_config_llama3_1_8B.json")

        except Exception as e:
            logger.error(f"Error getting config path for model {model_id}: {e}")
            return "genie_config_llama3_1_8B.json"

    @staticmethod
    def get_model_prompt_template(model_id: str) -> str:
        """
        Get the prompt template for a given model.

        Args:
            model_id: The model identifier (can be internal ID or external ID)

        Returns:
            str: The model's prompt template string with {role} and {content} placeholders
        """
        try:
            config_manager = get_config_manager()

            # Try as external model ID first
            prompt_template = config_manager.get_prompt_template(model_id)
            if prompt_template:
                logger.info(f"Found prompt template for model {model_id}")
                return prompt_template

            # Try to find by internal ID
            external_id = config_manager.get_model_by_internal_id(model_id)
            if external_id:
                prompt_template = config_manager.get_prompt_template(external_id)
                if prompt_template:
                    logger.info(f"Found prompt template for internal model {model_id} (external: {external_id})")
                    return prompt_template

            # Fallback to default template from config
            logger.warning(f"No prompt template found for model {model_id}, using fallback")
            fallback_template = config_manager.models_config.get("fallback_template",
                "<|begin_of_text|><|start_header_id|>{role}<|end_header_id|>\n\n{content}<|eot_id|><|start_header_id|>assistant<|end_header_id|>")
            return fallback_template

        except Exception as e:
            logger.error(f"Error getting prompt template for model {model_id}: {e}")
            # Hardcoded fallback
            return "<|begin_of_text|><|start_header_id|>{role}<|end_header_id|>\n\n{content}<|eot_id|><|start_header_id|>assistant<|end_header_id|>"

    @staticmethod
    def get_assistant_prompt(model_id: str) -> str:
        """
        Get the assistant prompt suffix for a given model.

        Args:
            model_id: The model identifier (can be internal ID or external ID)

        Returns:
            str: The assistant prompt string to append at the end
        """
        try:
            config_manager = get_config_manager()

            # Try as external model ID first
            model_config = config_manager.get_model_config(model_id)
            if model_config and 'assistant_prompt' in model_config:
                return model_config['assistant_prompt']

            # Try to find by internal ID
            external_id = config_manager.get_model_by_internal_id(model_id)
            if external_id:
                model_config = config_manager.get_model_config(external_id)
                if model_config and 'assistant_prompt' in model_config:
                    return model_config['assistant_prompt']

            # Fallback - return empty string
            logger.warning(f"No assistant prompt found for model {model_id}, using empty string")
            return ""

        except Exception as e:
            logger.error(f"Error getting assistant prompt for model {model_id}: {e}")
            return ""

    @staticmethod
    def format_message_with_template(model_id: str, role: str, content) -> str:
        """
        Format a message with the appropriate prompt template for the model.

        Args:
            model_id: The model identifier
            role: The message role (system, user, assistant)
            content: The message content (can be string or object)

        Returns:
            str: The formatted message content
        """
        template = CommonUtils.get_model_prompt_template(model_id)
        logger.info(f"Formatting message with template for model: {model_id}, role: {role}")

        try:
            # Handle both string and object content
            content_str = str(content) if not isinstance(content, str) else content

            # Replace placeholders in template
            formatted = template.replace("{role}", role).replace("{content}", content_str)
            logger.debug(f"Successfully formatted message for role '{role}' using model '{model_id}'")
            return formatted
        except Exception as e:
            logger.error(f"Error formatting message with template: {e}")
            # Return original content as fallback
            logger.warning("Returning original content without template formatting")
            return str(content) if not isinstance(content, str) else content
