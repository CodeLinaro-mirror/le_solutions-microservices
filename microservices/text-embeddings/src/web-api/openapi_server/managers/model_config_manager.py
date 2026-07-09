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
    Singleton manager for dynamically discovering and loading model configurations.
    Scans the models directory (/mnt/work/models/) for subdirectories that contain
    metadata.json and dynamically constructs the models dictionary, falling back
    to the baked-in models_config.json if no bundles are found.
    """

    _instance: Optional['ModelConfigManager'] = None
    _config: Optional[Dict[str, Any]] = None

    def __new__(cls):
        if cls._instance is None:
            cls._instance = super(ModelConfigManager, cls).__new__(cls)
            cls._instance._reload_count = 0
            cls._instance._last_mtimes = {}
        return cls._instance

    def _get_metadata_mtimes(self) -> Dict[str, float]:
        models_dir = os.getenv("T2E_MODEL_DIR", "/mnt/work/models")
        mtimes = {}
        if os.path.isdir(models_dir):
            try:
                # Check root directory first
                root_metadata = os.path.join(models_dir, "metadata.json")
                if os.path.isfile(root_metadata):
                    mtimes[root_metadata] = os.path.getmtime(root_metadata)

                # Check subdirectories
                for entry in os.scandir(models_dir):
                    if entry.is_dir():
                        sub_metadata = os.path.join(entry.path, "metadata.json")
                        if os.path.isfile(sub_metadata):
                            mtimes[sub_metadata] = os.path.getmtime(sub_metadata)
            except Exception as e:
                logger.warning(f"ModelConfigManager._get_metadata_mtimes: failed checking modification times: {e}")
        return mtimes

    def get_reload_count(self) -> int:
        return getattr(self, "_reload_count", 0)

    def _discover_models(self) -> Dict[str, Any]:
        """
        Scan T2E_MODEL_DIR (or subdirectories) containing metadata.json and dynamically
        construct model registrations.
        """
        models_dir = os.getenv("T2E_MODEL_DIR", "/mnt/work/models")
        logger.info(f"ModelConfigManager._discover_models: Scanning '{models_dir}' for aihub model bundles…")

        discovered_models: Dict[str, Any] = {}
        primary_runtime = ""
        model_id_prefix = "nomic_embed_text"

        if not os.path.isdir(models_dir):
            logger.warning(f"ModelConfigManager._discover_models: Models directory '{models_dir}' does not exist or is not a directory.")
        else:
            try:
                metadata_sources = []
                # Check root directory first
                root_metadata = os.path.join(models_dir, "metadata.json")
                if os.path.isfile(root_metadata):
                    logger.debug(f"ModelConfigManager._discover_models: Found metadata.json in root models directory: {root_metadata}")
                    metadata_sources.append(("", root_metadata))
                else:
                    logger.debug(f"ModelConfigManager._discover_models: No metadata.json found in root models directory.")

                # Check subdirectories as fallback
                logger.debug(f"ModelConfigManager._discover_models: Scanning subdirectories in {models_dir}...")
                for entry in os.scandir(models_dir):
                    if entry.is_dir():
                        sub_metadata = os.path.join(entry.path, "metadata.json")
                        if os.path.isfile(sub_metadata):
                            logger.debug(f"ModelConfigManager._discover_models: Found metadata.json in subdirectory '{entry.name}': {sub_metadata}")
                            metadata_sources.append((entry.name, sub_metadata))

                logger.info(f"ModelConfigManager._discover_models: Found {len(metadata_sources)} metadata source file(s) to process.")

                for prefix, metadata_path in metadata_sources:
                    logger.debug(f"ModelConfigManager._discover_models: Processing metadata source {metadata_path!r}")
                    try:
                        with open(metadata_path, encoding="utf-8") as f:
                            meta = json.load(f)
                    except Exception as exc:
                        logger.warning(f"ModelConfigManager._discover_models: Could not read '{metadata_path}': {exc} – skipping.")
                        continue

                    raw_id = meta.get("model_id", "")
                    if not raw_id:
                        logger.warning(f"ModelConfigManager._discover_models: 'model_id' key is missing or empty in {metadata_path} – skipping.")
                        continue
                    model_id_prefix = raw_id

                    runtime = meta.get("runtime", "")
                    if not primary_runtime:
                        primary_runtime = runtime
                    display_name = meta.get("model_name", raw_id)
                    model_files = meta.get("model_files", {})
                    if not model_files:
                        logger.warning(f"ModelConfigManager._discover_models: 'model_files' key is missing or empty in {metadata_path} – skipping.")
                        continue

                    first_model_file = list(model_files.keys())[0]
                    logger.debug(f"ModelConfigManager._discover_models: parsed model_id={raw_id!r}, runtime={runtime!r}, model_file={first_model_file!r}")

                    # Map dynamically to normalized model configurations
                    if runtime == "qnn_dlc" or runtime == "snpe":
                        # 1. QNN DLC model ID
                        qnn_dlc_id = f"{raw_id}_qnn_dlc"
                        qnn_dlc_file = os.path.join(prefix, first_model_file) if prefix else first_model_file
                        discovered_models[qnn_dlc_id] = {
                            "model_file": qnn_dlc_file,
                            "display_name": f"{display_name}-QNN-DLC",
                            "max_tokens": 128
                        }
                        logger.info(f"ModelConfigManager._discover_models: Registered QNN DLC model {qnn_dlc_id!r} -> {qnn_dlc_file!r}")

                        # 2. SNPE model ID (shares the same DLC)
                        snpe_id = f"{raw_id}_snpe"
                        snpe_file = os.path.join(prefix, first_model_file) if prefix else first_model_file
                        discovered_models[snpe_id] = {
                            "model_file": snpe_file,
                            "display_name": f"{display_name}-SNPE",
                            "max_tokens": 128
                        }
                        logger.info(f"ModelConfigManager._discover_models: Registered SNPE model {snpe_id!r} -> {snpe_file!r}")

                        # 3. QNN Binary model ID
                        qnn_id = f"{raw_id}_qnn"
                        bin_file = first_model_file.replace(".dlc", ".bin")
                        if os.path.isfile(os.path.join(models_dir, bin_file)):
                            bin_model_file = bin_file
                            logger.debug(f"ModelConfigManager._discover_models: QNN .bin file found in root: {bin_file}")
                        else:
                            bin_model_file = os.path.join(prefix, bin_file) if prefix else bin_file
                            logger.debug(f"ModelConfigManager._discover_models: QNN .bin file mapped with prefix: {bin_model_file}")

                        discovered_models[qnn_id] = {
                            "model_file": bin_model_file,
                            "display_name": f"{display_name}-QNN",
                            "max_tokens": 128
                        }
                        logger.info(f"ModelConfigManager._discover_models: Registered QNN model {qnn_id!r} -> {bin_model_file!r}")

                    elif runtime == "qnn_context_binary" or runtime == "qnn":
                        qnn_id = f"{raw_id}_qnn"
                        qnn_file = os.path.join(prefix, first_model_file) if prefix else first_model_file
                        discovered_models[qnn_id] = {
                            "model_file": qnn_file,
                            "display_name": f"{display_name}-QNN",
                            "max_tokens": 128
                        }
                        logger.info(f"ModelConfigManager._discover_models: Registered QNN Context Binary model {qnn_id!r} -> {qnn_file!r}")

                    elif runtime == "tflite" or runtime == "litert":
                        tflite_id = f"{raw_id}_tflite"
                        tflite_file = os.path.join(prefix, first_model_file) if prefix else first_model_file
                        discovered_models[tflite_id] = {
                            "model_file": tflite_file,
                            "display_name": f"{display_name}-TFLite",
                            "max_tokens": 128
                        }
                        logger.info(f"ModelConfigManager._discover_models: Registered LiteRT/TFLite model {tflite_id!r} -> {tflite_file!r}")
            except Exception as e:
                logger.error(f"ModelConfigManager._discover_models: Error during model discovery: {e}", exc_info=True)

        default_model = next(iter(discovered_models)) if discovered_models else ""

        # Determine default model based on the primary runtime
        if (primary_runtime == "tflite" or primary_runtime == "litert") and f"{model_id_prefix}_tflite" in discovered_models:
            default_model = f"{model_id_prefix}_tflite"
        elif primary_runtime == "snpe" and f"{model_id_prefix}_snpe" in discovered_models:
            default_model = f"{model_id_prefix}_snpe"
        elif primary_runtime == "qnn_dlc" and f"{model_id_prefix}_qnn_dlc" in discovered_models:
            default_model = f"{model_id_prefix}_qnn_dlc"
        elif (primary_runtime == "qnn" or primary_runtime == "qnn_context_binary") and f"{model_id_prefix}_qnn" in discovered_models:
            default_model = f"{model_id_prefix}_qnn"
        elif "nomic_embed_text_qnn" in discovered_models:
            default_model = "nomic_embed_text_qnn"

        # Increment reload count to signal dependent caching services
        self._reload_count = getattr(self, "_reload_count", 0) + 1

        logger.info(f"ModelConfigManager._discover_models: Discovery completed (reload count={self._reload_count}). Total discovered models={len(discovered_models)}, Default model={default_model!r}, Model list={list(discovered_models.keys())}")
        return {
            "models": discovered_models,
            "default_model": default_model
        }

    def get_config(self, force_reload: bool = False) -> Dict[str, Any]:
        """
        Get the model configuration. Loads from file on first call, if force_reload is True,
        or if any metadata.json files have been modified at runtime.

        Args:
            force_reload: If True, reload the configuration from file even if cached.

        Returns:
            Dictionary containing the model configuration.
        """
        current_mtimes = self._get_metadata_mtimes()

        # Check if anything changed on disk at runtime
        if not force_reload and self._config is None:
            pass # continue loading
        elif not force_reload and self._config is not None:
            if current_mtimes != getattr(self, "_last_mtimes", {}):
                logger.info("ModelConfigManager.get_config: Changes detected in metadata.json files at runtime. Forcing configuration reload.")
                force_reload = True

        if force_reload or self._config is None:
            self._config = self._discover_models()
            self._last_mtimes = current_mtimes

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
        return config.get("default_model", "nomic_embed_text_qnn")

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
