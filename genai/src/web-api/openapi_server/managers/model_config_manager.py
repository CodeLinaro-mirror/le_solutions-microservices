# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import json
import os
import pathlib
import threading
import glob
import shutil
from typing import Dict, List, Optional
from openapi_server.logger.logger_config import LoggerConfig

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)


class ModelConfigManager:
    """
    Singleton class to manage model configurations from bundle files.
    Scans model bundles, updates configuration paths, and provides model access.
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
            self.models_dir = os.getenv("GENAI_MODELS_DIR", "/mnt/work/models")
            self.tmp_config_dir = "/tmp/configs"
            self.models_config = self._scan_model_bundles()
            self._initialized = True
            logger.info(f"ModelConfigManager initialized with models from: {self.models_dir}")

    def _scan_model_bundles(self) -> Dict:
        """
        Scan model bundles in the models directory.
        Copies and modifies configuration files to support absolute paths.

        Returns:
            Dict: The aggregated model configuration
        """
        aggregated_config = {
            "models": {},
            "default_model": None,
            "fallback_chat_template": {
                "global_prefix": "",
                "system_prefix": "<|begin_of_text|><|start_header_id|>system<|end_header_id|>\n\n",
                "system_suffix": "<|eot_id|>",
                "user_prefix": "<|start_header_id|>user<|end_header_id|>\n\n",
                "user_suffix": "<|eot_id|>",
                "assistant_prefix": "<|start_header_id|>assistant<|end_header_id|>\n\n",
                "assistant_suffix": "<|eot_id|>",
                "default_system_prompt": "You are a helpful assistant."
            }
        }

        if not os.path.exists(self.models_dir):
            logger.warning(f"Models directory not found: {self.models_dir}")
            return aggregated_config

        # Clean up tmp config dir if it exists
        if os.path.exists(self.tmp_config_dir):
            shutil.rmtree(self.tmp_config_dir)
        os.makedirs(self.tmp_config_dir, exist_ok=True)

        for root, dirs, files in os.walk(self.models_dir):
            if "model_config.json" in files:
                bundle_path = root
                bundle_name = os.path.basename(bundle_path)
                model_config_path = os.path.join(bundle_path, "model_config.json")

                try:
                    # Process the bundle (copy configs, update paths)
                    processed_config_dir = self._process_bundle(bundle_path, bundle_name)

                    # Load the model metadata from the PROCESSED config (which has capping applied)
                    processed_model_config_path = os.path.join(processed_config_dir, "model_config.json")
                    with open(processed_model_config_path, 'r') as f:
                        model_config_data = json.load(f)

                    for model_id, model_info in model_config_data.get("models", {}).items():
                        # Update config_file to point to the processed copy in /tmp/configs
                        original_config_file = model_info.get("config_file")
                        if original_config_file:
                            model_info["config_file"] = os.path.join(processed_config_dir, original_config_file)

                        aggregated_config["models"][model_id] = model_info

                        # Set default model if not set
                        if aggregated_config["default_model"] is None:
                            aggregated_config["default_model"] = model_id

                    logger.info(f"Loaded models from bundle: {bundle_name} at {bundle_path}")

                except Exception as e:
                    logger.error(f"Error processing bundle {bundle_name} at {bundle_path}: {e}")

        return aggregated_config

    def _process_bundle(self, bundle_path: str, bundle_name: str) -> str:
        """
        Process a single bundle: copy JSON configs to tmp dir and update paths.

        Args:
            bundle_path: Path to the original bundle
            bundle_name: Name of the bundle

        Returns:
            str: Path to the directory containing processed configs
        """
        output_dir = os.path.join(self.tmp_config_dir, bundle_name)
        os.makedirs(output_dir, exist_ok=True)

        # Find all JSON files in the bundle
        json_files = glob.glob(os.path.join(bundle_path, "*.json"))

        for json_file in json_files:
            filename = os.path.basename(json_file)

            # Skip tokenizer.json as per requirement
            if filename == "tokenizer.json":
                continue

            # Read original JSON
            with open(json_file, 'r') as f:
                try:
                    data = json.load(f)
                except json.JSONDecodeError:
                    logger.warning(f"Failed to parse JSON file: {json_file}")
                    continue

            # NOTE: GenIE/qualla's sampler latches greedy mode at construction time when top-k == 1.
            # Runtime sampler updates via GenieSampler_applyConfig update temp/top-k/top-p, but do not
            # recompute the internal greedy flag. This means a model bundle with dialog.sampler.top-k=1
            # behaves greedily even if the request tries to override top-k > 1.
            #
            # Workaround: ensure dialog sampler does not start with top-k == 1 so runtime overrides work.
            self._patch_dialog_sampler_config(data, bundle_name=bundle_name, filename=filename)

            # Apply context capping if configured
            self._apply_context_capping(data, bundle_name, filename)

            # Update paths in the JSON data
            updated_data = self._update_paths(data, bundle_path, output_dir)

            # Write to output directory
            output_path = os.path.join(output_dir, filename)
            with open(output_path, 'w') as f:
                json.dump(updated_data, f, indent=4)

        return output_dir

    @staticmethod
    def _apply_context_capping(data: dict, bundle_name: str, filename: str) -> None:
        """
        Apply context size capping if GENAI_CONTEXT_CAPPING env var is set.
        Updates dialog.context.size or context.size to be min(original, cap).
        """
        try:
            cap_str = os.getenv("GENAI_CONTEXT_CAPPING")
            if not cap_str:
                return

            try:
                cap_size = int(cap_str)
            except ValueError:
                logger.warning(f"Invalid GENAI_CONTEXT_CAPPING value: {cap_str}")
                return

            # Check all known root node keys that may contain context.size
            # Covers LLM configs ("dialog") and VLM text-generator node configs
            for node_key in ("dialog", "text-generator", "text_generator", "textGenerator"):
                node = data.get(node_key)
                if isinstance(node, dict):
                    context = node.get("context")
                    if isinstance(context, dict):
                        original_size = context.get("size")
                        if isinstance(original_size, int) and original_size > cap_size:
                            context["size"] = cap_size
                            logger.warning(
                                f"[CONTEXT OVERRIDE] GENAI_CONTEXT_CAPPING applied to "
                                f"{bundle_name}/{filename}: "
                                f"{node_key}.context.size {original_size} -> {cap_size}"
                            )

            # Check for top-level context -> size (model_config.json)
            context = data.get("context")
            if isinstance(context, dict):
                original_size = context.get("size")
                if isinstance(original_size, int) and original_size > cap_size:
                    context["size"] = cap_size
                    logger.warning(
                        f"[CONTEXT OVERRIDE] GENAI_CONTEXT_CAPPING applied to "
                        f"{bundle_name}/{filename}: "
                        f"context.size {original_size} -> {cap_size}"
                    )

        except Exception as e:
            logger.warning(f"Error applying context capping: {e}")

    @staticmethod
    def _patch_dialog_sampler_config(data: dict, bundle_name: str, filename: str) -> None:
        """
        Patch dialog sampler config in-place to ensure runtime sampling overrides work.

        GenIE/qualla sets internal greedy mode when initializing a basic sampler with top-k == 1.
        Greedy mode is not updated on later applyConfig() calls, so runtime top-k overrides are
        effectively ignored if the sampler starts with top-k == 1.
        """
        try:
            # LLM dialog config
            dialog = data.get("dialog")
            if isinstance(dialog, dict):
                sampler = dialog.get("sampler")
                if isinstance(sampler, dict) and sampler.get("top-k") == 1:
                    sampler["top-k"] = 2
                    logger.warning(
                        f"Patched dialog.sampler.top-k from 1 to 2 in {bundle_name}/{filename} "
                        "to allow runtime sampling overrides (GenIE greedy mode is latched when top-k==1)."
                    )

            # VLM/text-generator node config (common in VLM bundles)
            for node_key in ("text-generator", "text_generator", "textGenerator"):
                node = data.get(node_key)
                if not isinstance(node, dict):
                    continue
                sampler = node.get("sampler")
                if isinstance(sampler, dict) and sampler.get("top-k") == 1:
                    sampler["top-k"] = 2
                    logger.warning(
                        f"Patched {node_key}.sampler.top-k from 1 to 2 in {bundle_name}/{filename} "
                        "to allow runtime sampling overrides (GenIE greedy mode is latched when top-k==1)."
                    )
        except Exception:
            # Best-effort patching; do not fail bundle loading due to unexpected config shapes.
            return

    def _update_paths(self, data, bundle_path: str, output_dir: str):
        """
        Recursively update paths in JSON data.

        Args:
            data: The JSON data (dict, list, or value)
            bundle_path: Path to the original bundle (for binaries/tokenizer)
            output_dir: Path to the processed config directory (for other JSONs)

        Returns:
            The data with updated paths
        """
        if isinstance(data, dict):
            return {k: self._update_paths(v, bundle_path, output_dir) for k, v in data.items()}
        elif isinstance(data, list):
            return [self._update_paths(item, bundle_path, output_dir) for item in data]
        elif isinstance(data, str):
            # Check if the string matches a file in the bundle
            potential_path = os.path.join(bundle_path, data)
            if os.path.isfile(potential_path):
                filename = os.path.basename(data)

                # If it's tokenizer.json, point to the original bundle path (since we didn't copy it)
                if filename == "tokenizer.json":
                    return potential_path

                # If it's a JSON file (and not tokenizer), point to the copy in output_dir
                if filename.endswith(".json"):
                    # We can use the absolute path to the copy
                    return os.path.join(output_dir, filename)

                # If it's any other file (binary, raw, etc.), point to the original bundle path
                return potential_path

            return data
        else:
            return data

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

    def get_context_size(self, model_id: str) -> int:
        """
        Get the context window size for a specific model.
        Respects GENAI_CONTEXT_CAPPING as a hard upper bound, even if the
        model's model_config.json does not define a top-level context.size
        (in which case _apply_context_capping would not have patched it).

        Args:
            model_id: The model identifier

        Returns:
            int: Context window size in tokens (capped if GENAI_CONTEXT_CAPPING is set)
        """
        model_config = self.get_model_config(model_id)
        size = 4096  # Default fallback
        if model_config and 'context' in model_config:
            size = model_config['context'].get('size', 4096)

        # Safety net: always honour GENAI_CONTEXT_CAPPING regardless of whether
        # model_config.json defines context.size (some bundles only define it in
        # the engine config, e.g. genie_config.json, not in model_config.json).
        cap_str = os.getenv("GENAI_CONTEXT_CAPPING")
        if cap_str:
            try:
                size = min(size, int(cap_str))
            except ValueError:
                pass

        return size

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

    def get_vision_preprocessing(self, model_id: str) -> Optional[Dict]:
        """
        Get the vision preprocessing configuration for a specific model.

        Args:
            model_id: The model identifier

        Returns:
            Dict containing vision preprocessing parameters or None if not found
        """
        model_config = self.get_model_config(model_id)
        if model_config:
            return model_config.get('vision_preprocessing')
        return None

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
        self.models_config = self._scan_model_bundles()

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

    def get_memory_requirement_mb(self, model_id: str) -> int:
        """
        Get the memory requirement in MB for a specific model.

        Args:
            model_id: The model identifier

        Returns:
            int: Memory requirement in megabytes, or 4096 as default
        """
        model_config = self.get_model_config(model_id)
        if model_config:
            return model_config.get('memory_requirement_mb', 4096)
        return 4096  # Default fallback


def get_config_manager() -> ModelConfigManager:
    """
    Get the ModelConfigManager singleton instance.

    Returns:
        ModelConfigManager: The singleton instance
    """
    return ModelConfigManager()
