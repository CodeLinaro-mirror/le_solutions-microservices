# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import os
from openapi_server.impl.constant import LLMServiceQueryConstant as QUERY_CONST

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
            str: The internal model ID (e.g., LLAMA3_1_8B)
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

        # Try to get from configuration
        try:
            config_manager = ModelConfigManager()
            default_model_id = config_manager.get_default_model()
            internal_id = config_manager.get_internal_id(default_model_id)

            if internal_id:
                logger.info(f"Using default model from config: {default_model_id} (internal: {internal_id})")
                return internal_id
            else:
                logger.warning(f"No internal_id found for model {default_model_id}, using fallback")
                return QUERY_CONST.DEFAULT_MODEL
        except Exception as e:
            logger.error(f"Error getting model from config: {e}, using fallback")
            return QUERY_CONST.DEFAULT_MODEL

    @staticmethod
    def get_model_config_path(model_id: str) -> str:
        """
        Get the configuration file path for a given model.

        Args:
            model_id: The model identifier (can be internal ID or external ID)

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
