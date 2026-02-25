# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import json
import os
import pathlib
import threading
from typing import Dict, List, Optional
from openapi_server.logger.logger_config import LoggerConfig

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)


class ModelConfigManager:
    """
    Singleton class to manage model configurations from a JSON file.
    Provides methods to access model properties, validate models, and get available models.
    """
    _instance = None
    _lock = threading.Lock()

    def __new__(cls):
        if cls._instance is None:
            with cls._lock:
                if cls._instance is None:
                    cls._instance = super().__new__(cls)
                    cls._instance._initialized = False
        return cls._instance

    def __init__(self):
        """Initialize the ModelConfigManager singleton."""
        if not self._initialized:
            # Default path is relative to this file's location
            default_config_path = pathlib.Path(__file__).parent.parent / "configs" / "models_config.json"
            self.config_path = os.getenv(
                "GENAI_MODELS_CONFIG_PATH",
                str(default_config_path)
            )
            self.models_config = self._load_models_config()
            self._initialized = True
            logger.info(f"ModelConfigManager initialized with config from: {self.config_path}")

    def _load_models_config(self) -> Dict:
        """
        Load model configuration from JSON file.
        Creates a default configuration if file doesn't exist.

        Returns:
            Dict: The loaded or default model configuration
        """
        if not os.path.exists(self.config_path):
            logger.warning(f"Model config file not found at {self.config_path}, creating default config")
            default_config = {
                "models": {
                    "llama3-8b": {
                        "config_file": "genie_config_llama3_1_8B.json",
                        "display_name": "Llama 3.1 8B",
                        "chat_template": {
                            "system_prefix": "<|begin_of_text|><|start_header_id|>system<|end_header_id|>\n\n",
                            "system_suffix": "<|eot_id|>",
                            "user_prefix": "<|start_header_id|>user<|end_header_id|>\n\n",
                            "user_suffix": "<|eot_id|>",
                            "assistant_prefix": "<|start_header_id|>assistant<|end_header_id|>\n\n",
                            "assistant_suffix": "<|eot_id|>",
                            "default_system_prompt": "You are a helpful assistant."
                        },
                        "max_tokens": 4096,
                        "supports_streaming": True,
                        "internal_id": "LLAMA3_1_8B"
                    }
                },
                "default_model": "llama3-8b",
                "fallback_chat_template": {
                    "system_prefix": "<|begin_of_text|><|start_header_id|>system<|end_header_id|>\n\n",
                    "system_suffix": "<|eot_id|>",
                    "user_prefix": "<|start_header_id|>user<|end_header_id|>\n\n",
                    "user_suffix": "<|eot_id|>",
                    "assistant_prefix": "<|start_header_id|>assistant<|end_header_id|>\n\n",
                    "assistant_suffix": "<|eot_id|>",
                    "default_system_prompt": "You are a helpful assistant."
                }
            }

            # Create directory if it doesn't exist
            os.makedirs(os.path.dirname(self.config_path), exist_ok=True)

            try:
                with open(self.config_path, 'w') as f:
                    json.dump(default_config, f, indent=2)
                logger.info(f"Created default model configuration at: {self.config_path}")
            except Exception as e:
                logger.error(f"Failed to create default config file: {e}")

            return default_config

        try:
            with open(self.config_path, 'r') as f:
                config = json.load(f)
                logger.info(f"Successfully loaded model config with {len(config.get('models', {}))} models")
                return config
        except Exception as e:
            logger.error(f"Error loading model config from {self.config_path}: {e}")
            raise

    def get_model_config(self, model_id: str) -> Optional[Dict]:
        """
        Get configuration for a specific model.

        Args:
            model_id: The model identifier

        Returns:
            Dict containing model configuration or None if not found
        """
        return self.models_config.get("models", {}).get(model_id)

    def get_available_models(self) -> List[Dict]:
        """
        Get list of all available models in OpenAI-compatible format.

        Returns:
            List of model dictionaries with id, object, and owned_by fields
        """
        models = []
        for model_id, config in self.models_config.get("models", {}).items():
            models.append({
                "id": model_id,
                "object": "model",
                "owned_by": "",
                "display_name": config.get("display_name", model_id),
                "max_tokens": config.get("max_tokens", 4096),
                "supports_streaming": config.get("supports_streaming", True)
            })
        return models

    def get_default_model(self) -> str:
        """
        Get the default model ID.

        Returns:
            str: The default model identifier
        """
        return self.models_config.get("default_model", "llama3-8b")

    def validate_model(self, model_id: str) -> bool:
        """
        Check if a model ID exists in the configuration.

        Args:
            model_id: The model identifier to validate

        Returns:
            bool: True if model exists, False otherwise
        """
        return model_id in self.models_config.get("models", {})

    def get_config_file_path(self, model_id: str) -> Optional[str]:
        """
        Get the Genie config file path for a specific model.

        Args:
            model_id: The model identifier

        Returns:
            str: Path to the config file or None if model not found
        """
        model_config = self.get_model_config(model_id)
        if model_config:
            return model_config.get("config_file")
        return None

    def get_chat_template(self, model_id: str) -> Optional[Dict]:
        """
        Get the chat template configuration for a specific model.

        Args:
            model_id: The model identifier

        Returns:
            Dict containing chat template components or None if not found
        """
        model_config = self.get_model_config(model_id)
        if model_config:
            return model_config.get('chat_template')
        return None

    def get_internal_id(self, model_id: str) -> Optional[str]:
        """
        Get the internal model ID (used for C++ layer) for a given model.

        Args:
            model_id: The model identifier

        Returns:
            str: The internal model ID or None if not found
        """
        model_config = self.get_model_config(model_id)
        if model_config:
            return model_config.get("internal_id")
        return None

    def get_model_by_internal_id(self, internal_id: str) -> Optional[str]:
        """
        Get the model ID from an internal ID.

        Args:
            internal_id: The internal model identifier (e.g., LLAMA3_1_8B)

        Returns:
            str: The model ID or None if not found
        """
        for model_id, config in self.models_config.get("models", {}).items():
            if config.get("internal_id") == internal_id:
                return model_id
        return None

    def get_context_size(self, model_id: str) -> int:
        """
        Get the context window size for a specific model.

        Args:
            model_id: The model identifier

        Returns:
            int: Context window size in tokens
        """
        model_config = self.get_model_config(model_id)
        if model_config and 'context' in model_config:
            return model_config['context'].get('size', 4096)
        return 4096  # Default fallback

    def get_summarization_threshold(self, model_id: str) -> float:
        """
        Get the summarization threshold for a specific model.

        Args:
            model_id: The model identifier

        Returns:
            float: Summarization threshold (e.g., 0.7 for 70%)
        """
        model_config = self.get_model_config(model_id)
        if model_config and 'context' in model_config:
            return model_config['context'].get('summarization_threshold', 0.7)
        return 0.7  # Default to 70%

    def supports_vision(self, model_id: str) -> bool:
        """
        Check if a model supports vision/image inputs.

        Args:
            model_id: The model identifier

        Returns:
            bool: True if model supports vision, False otherwise
        """
        model_config = self.get_model_config(model_id)
        if model_config:
            return model_config.get('supports_vision', False)
        return False

    def reload_config(self):
        """Reload the configuration from file."""
        logger.info("Reloading model configuration")
        self.models_config = self._load_models_config()

    def has_vision_models(self) -> bool:
        """
        Check if any configured models support vision.

        Returns:
            bool: True if any model has 'supports_vision': true, False otherwise
        """
        try:
            for model_id, model_config in self.models_config.get("models", {}).items():
                if model_config.get("supports_vision", False):
                    logger.debug(f"Vision model detected: {model_id}")
                    return True
            logger.debug("No vision models found in configuration")
            return False
        except Exception as e:
            logger.error(f"Error checking for vision models: {e}")
            return False


def get_config_manager() -> ModelConfigManager:
    """
    Get the ModelConfigManager singleton instance.

    Returns:
        ModelConfigManager: The singleton instance
    """
    return ModelConfigManager()
