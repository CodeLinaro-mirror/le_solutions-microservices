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
    Looks for models_config.json in the mounted models directory
    (/mnt/work/models/models_config.json), falling back to the
    baked-in default (openapi_server/configs/models_config.json).
    """

    _instance: Optional['ModelConfigManager'] = None
    _config: Optional[Dict[str, Any]] = None

    def __new__(cls):
        if cls._instance is None:
            cls._instance = super(ModelConfigManager, cls).__new__(cls)
        return cls._instance

    def _load_config(self) -> Dict[str, Any]:
        """Load configuration from file.

        Lookup order:
          1. /mnt/work/models/models_config.json  (auto-discovered from mounted models dir)
          2. openapi_server/configs/models_config.json  (baked-in fallback)
        """
        models_dir_config = "/mnt/work/models/models_config.json"
        fallback_config = os.path.join(
            os.path.dirname(os.path.dirname(__file__)),
            "configs",
            "models_config.json"
        )

        if os.path.isfile(models_dir_config):
            config_path = models_dir_config
            logger.info(f"Found models_config.json in models directory. Loading from: {config_path}")
        else:
            config_path = fallback_config
            logger.info(f"models_config.json not found in models directory. Using default: {config_path}")

        try:
            with open(config_path, "r") as f:
                config = json.load(f)
            logger.info(f"Successfully loaded model configuration from {config_path}")
            return config
        except FileNotFoundError:
            logger.error(f"Model configuration file not found: {config_path}")
            raise FileNotFoundError(f"Model configuration file not found: {config_path}")
        except json.JSONDecodeError as e:
            logger.error(f"Invalid JSON in model configuration file: {e}")
            raise ValueError(f"Invalid JSON in model configuration file: {e}")
        except Exception as e:
            logger.error(f"Failed to read model configuration file: {e}")
            raise Exception(f"Failed to read model configuration file: {e}")

    def get_config(self, force_reload: bool = False) -> Dict[str, Any]:
        """
        Get the model configuration. Loads from file on first call or if force_reload is True.

        Args:
            force_reload: If True, reload the configuration from file even if cached.

        Returns:
            Dictionary containing the model configuration.
        """
        # Reload if forced or config not yet loaded
        if force_reload or self._config is None:
            self._config = self._load_config()

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

        Returns:
            Default model ID string.
        """
        config = self.get_config()
        return config.get("default_model", "nomic-embed-text")

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


# Create a singleton instance
model_config_manager = ModelConfigManager()
