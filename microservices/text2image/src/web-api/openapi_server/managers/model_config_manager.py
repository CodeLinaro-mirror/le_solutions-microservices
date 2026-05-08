# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import os
import json
from typing import Dict, Any, Optional
from openapi_server.logger.logger_config import LoggerConfig

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)


class ModelConfigManager:
    """
    Singleton manager for loading and caching model configuration.
    Reads from the model_config.json file specified by GENAI_MODELS_CONFIG_PATH.
    """
    
    _instance: Optional['ModelConfigManager'] = None
    _config: Optional[Dict[str, Any]] = None
    _config_path: Optional[str] = None
    
    def __new__(cls):
        if cls._instance is None:
            cls._instance = super(ModelConfigManager, cls).__new__(cls)
        return cls._instance
    
    def _load_config(self) -> Dict[str, Any]:
        """Load configuration from file."""
        user_provided_path = os.getenv("GENAI_MODELS_CONFIG_PATH")
        config_path = user_provided_path or "/app/default_configs/models_config.json"

        if user_provided_path:
            logger.info(f"GENAI_MODELS_CONFIG_PATH is set. Loading model configuration from: {config_path}")
        else:
            logger.info(f"GENAI_MODELS_CONFIG_PATH is not set. Using default model configuration: {config_path}")

        try:
            with open(config_path, "r") as f:
                config = json.load(f)
            logger.info(f"Successfully loaded model configuration from {config_path}")
            return config
        except FileNotFoundError:
            if user_provided_path:
                logger.error(
                    f"Model configuration file not found at user-provided path "
                    f"GENAI_MODELS_CONFIG_PATH='{config_path}'. "
                    f"Please ensure the file exists and is accessible."
                )
                raise FileNotFoundError(
                    f"Model configuration file not found at user-provided path "
                    f"GENAI_MODELS_CONFIG_PATH='{config_path}'. "
                    f"Please ensure the file exists and is accessible."
                )
            else:
                logger.error(f"Default model configuration file not found: {config_path}")
                raise FileNotFoundError(f"Default model configuration file not found: {config_path}")
        except json.JSONDecodeError as e:
            logger.error(f"Invalid JSON in model configuration file '{config_path}': {e}")
            raise ValueError(f"Invalid JSON in model configuration file '{config_path}': {e}")
        except Exception as e:
            logger.error(f"Failed to read model configuration file '{config_path}': {e}")
            raise Exception(f"Failed to read model configuration file '{config_path}': {e}")
    
    def get_config(self, force_reload: bool = False) -> Dict[str, Any]:
        """
        Get the model configuration. Loads from file on first call or if force_reload is True.
        
        Args:
            force_reload: If True, reload the configuration from file even if cached.
            
        Returns:
            Dictionary containing the model configuration.
        """
        current_config_path = os.getenv("GENAI_MODELS_CONFIG_PATH", "/app/default_configs/models_config.json")
        
        # Reload if forced, config not loaded, or config path changed
        if force_reload or self._config is None or self._config_path != current_config_path:
            self._config = self._load_config()
            self._config_path = current_config_path
        
        return self._config
    
    def get_models(self) -> Dict[str, Any]:
        """
        Get the models dictionary from configuration.
        
        Returns:
            Dictionary of available models with model IDs as keys.
        """
        config = self.get_config()
        return config.get("models", {})
    
    def get_default_model(self) -> str:
        """
        Get the default model ID from configuration.
        Returns the first model listed in the 'models' section of the config.
        
        Returns:
            Default model ID string.
        """
        models = self.get_models()
        if not models:
            raise ValueError("No models available in configuration.")
        return next(iter(models))
    
    def get_model_info(self, model_id: str) -> Optional[Dict[str, Any]]:
        """
        Get information about a specific model.
        
        Args:
            model_id: The model identifier.
            
        Returns:
            Dictionary containing model information, or None if model not found.
        """
        models = self.get_models()
        return models.get(model_id)
    
    def is_model_available(self, model_id: str) -> bool:
        """
        Check if a model is available in the configuration.
        
        Args:
            model_id: The model identifier to check.
            
        Returns:
            True if model exists, False otherwise.
        """
        return model_id in self.get_models()
    
    def get_available_model_ids(self) -> list:
        """
        Get list of all available model IDs.
        
        Returns:
            List of model ID strings.
        """
        return list(self.get_models().keys())
    
    def extract_variant_from_model_id(self, model_id: str) -> str:
        """
        Extract the variant from a model ID.
        For example: "stable-diffusion-2.1" -> "2.1"
        
        Args:
            model_id: The model identifier.
            
        Returns:
            Variant string (e.g., "2.1", "1.5").
        """
        model_parts = model_id.split("-")
        variant = model_parts[-1] if len(model_parts) > 0 else "2.1"
        logger.debug(f"Extracted variant '{variant}' from model ID '{model_id}'")
        return variant


# Create a singleton instance
model_config_manager = ModelConfigManager()
