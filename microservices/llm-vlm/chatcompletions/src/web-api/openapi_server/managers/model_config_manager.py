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
            if "metadata.json" in files:
                bundle_path = root
                bundle_name = os.path.basename(bundle_path)

                try:
                    # Process the bundle (copy configs, update paths)
                    processed_config_dir = self._process_bundle(bundle_path, bundle_name)

                    # Load the PROCESSED metadata.json (paths already made absolute)
                    processed_metadata_path = os.path.join(processed_config_dir, "metadata.json")
                    with open(processed_metadata_path, 'r') as f:
                        metadata = json.load(f)

                    model_id = metadata.get("model_id")
                    if model_id:
                        model_config = self._parse_metadata_json(metadata, bundle_path, processed_config_dir)
                        aggregated_config["models"][model_id] = model_config

                        # Set default model if not set
                        if aggregated_config["default_model"] is None:
                            aggregated_config["default_model"] = model_id

                        logger.info(f"Loaded metadata model: {model_id} from bundle: {bundle_name}")
                    else:
                        logger.warning(f"No model_id found in metadata.json in bundle {bundle_name}")

                except Exception as e:
                    logger.error(f"Error processing metadata bundle {bundle_name} at {bundle_path}: {e}")

            elif "model_config.json" in files:
                bundle_path = root
                bundle_name = os.path.basename(bundle_path)

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

    def _parse_metadata_json(self, metadata: dict, bundle_path: str, processed_config_dir: str) -> dict:
        """
        Parse metadata.json into internal model configuration format.
        """
        from openapi_server.impl.constant import SUMMARIZATION_THRESHOLD

        genie = metadata.get("genie", {})
        supports_vision = genie.get("supports_vision", False)
        context_lengths = genie.get("context_lengths", [4096])
        context_size = max(context_lengths) if context_lengths else 4096
        pipeline_nodes = genie.get("pipeline", {}).get("nodes", {})

        if supports_vision:
            config_file = self._generate_vlm_genie_config(metadata, genie, pipeline_nodes, processed_config_dir)
        else:
            config_file = pipeline_nodes.get("textGenerator", os.path.join(processed_config_dir, "genie_config.json"))

        memory_mb = self._calculate_memory_from_bin_files(bundle_path, metadata.get("model_files", {}))

        model_config = {
            "config_file": config_file,
            "display_name": metadata.get("model_name", metadata.get("model_id", "Unknown")),
            "memory_requirement_mb": memory_mb,
            "chat_template": genie.get("chat_template", {}),
            "max_tokens": context_size,
            "supports_streaming": genie.get("supports_streaming", True),
            "supports_vision": supports_vision,
            "context": {
                "size": context_size,
                "summarization_threshold": SUMMARIZATION_THRESHOLD
            }
        }

        vision_preprocessing = genie.get("vision_preprocessing")
        if vision_preprocessing:
            model_config["vision_preprocessing"] = vision_preprocessing

        return model_config

    def _generate_vlm_genie_config(self, metadata: dict, genie: dict, nodes: dict, processed_config_dir: str) -> str:
        """
        Generate a synthetic genie_config.json for VLM models.
        Matches the structure vlm-service.cpp expects.
        """
        model_name = metadata.get("model_name", "VLM")

        pipeline_nodes = {}
        for key in ("imageEncoder", "lutEncoder", "textGenerator"):
            if key in nodes:
                pipeline_nodes[key] = nodes[key]

        DYNAMIC_TYPES = {
            "GENIE_NODE_IMAGE_ENCODER_IMAGE_INPUT",
            "GENIE_NODE_TEXT_ENCODER_TEXT_INPUT",
            "GENIE_NODE_TEXT_GENERATOR_TEXT_INPUT",
        }

        custom_inputs = []
        for sample in genie.get("sample_inputs", []):
            input_type = sample.get("input_type") or sample.get("node_io")
            if input_type not in DYNAMIC_TYPES:
                # Resolve file path to the absolute path in the original bundle directory
                # We can deduce the bundle path from processed_config_dir which is /tmp/configs/bundle_name
                bundle_name = os.path.basename(processed_config_dir)
                original_bundle_path = os.path.join(self.models_dir, bundle_name)

                # Make path absolute
                file_path = sample.get("file")
                if file_path and not os.path.isabs(file_path):
                    file_path = os.path.join(original_bundle_path, file_path)

                custom_inputs.append({
                    "node": sample.get("node"),
                    "input_type": input_type,
                    "file": file_path
                })

        chat_template = genie.get("chat_template", {})

        model_entry = {
            "description": f"{model_name} Vision-Language Model",
            "pipeline": {
                "nodes": pipeline_nodes
            },
            "custom_inputs": custom_inputs
        }

        if "vision_start" in chat_template:
            model_entry["vision_start_token"] = chat_template["vision_start"]
        if "vision_end" in chat_template:
            model_entry["vision_end_token"] = chat_template["vision_end"]

        genie_config = {
            model_name: model_entry
        }

        output_path = os.path.join(processed_config_dir, "generated_genie_config.json")
        with open(output_path, 'w') as f:
            json.dump(genie_config, f, indent=2)

        logger.info(f"Generated VLM genie_config at: {output_path}")
        return output_path

    def _calculate_memory_from_bin_files(self, bundle_path: str, model_files: dict) -> int:
        """
        Calculate memory requirement from bin file sizes.
        Returns total size of all .bin files * 1.25, in MB.
        """
        total_bytes = 0

        if model_files:
            for filename in model_files.keys():
                file_path = os.path.join(bundle_path, filename)
                if os.path.isfile(file_path):
                    total_bytes += os.path.getsize(file_path)
                else:
                    logger.warning(f"Bin file not found: {file_path}")
        else:
            for f in os.listdir(bundle_path):
                if f.endswith('.bin'):
                    file_path = os.path.join(bundle_path, f)
                    total_bytes += os.path.getsize(file_path)

        if total_bytes == 0:
            logger.warning(f"No bin files found in {bundle_path}, using default memory estimate")
            return 4096

        total_mb = int((total_bytes / (1024 * 1024)) * 1.25)
        logger.info(f"Calculated memory requirement: {total_mb}MB from {total_bytes} bytes of bin files")
        return total_mb

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

        # Determine LLM config filename from metadata.json if present
        llm_config_filename = None
        metadata_path = os.path.join(bundle_path, "metadata.json")
        if os.path.exists(metadata_path):
            try:
                with open(metadata_path, 'r') as f:
                    metadata = json.load(f)
                    genie = metadata.get("genie", {})
                    if not genie.get("supports_vision", False):
                        pipeline_nodes = genie.get("pipeline", {}).get("nodes", {})
                        llm_config_filename = pipeline_nodes.get("textGenerator")
            except Exception:
                pass

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

            # Normalize LLM dialog config: replace non-"dialog" root keys with "dialog"
            if llm_config_filename and filename == llm_config_filename and "dialog" not in data and len(data) == 1:
                old_key = next(iter(data.keys()))
                data["dialog"] = data.pop(old_key)
                logger.info(
                    f"[LLM CONFIG NORMALIZATION] Replaced root key '{old_key}' with 'dialog' "
                    f"in {bundle_name}/{filename} for Genie Dialog API compatibility"
                )

            # NOTE: GenIE/qualla's sampler latches greedy mode at construction time when top-k == 1.
            # Runtime sampler updates via GenieSampler_applyConfig update temp/top-k/top-p, but do not
            # recompute the internal greedy flag. This means a model bundle with dialog.sampler.top-k=1
            # behaves greedily even if the request tries to override top-k > 1.
            #
            # Workaround: ensure dialog sampler does not start with top-k == 1 so runtime overrides work.
            self._patch_dialog_sampler_config(data, bundle_name=bundle_name, filename=filename)

            # Apply context capping if configured
            self._apply_context_capping(data, bundle_name, filename)

            # Override QnnHtp polling behavior to false to prevent idle CPU usage in background service
            self._patch_htp_polling_config(data, bundle_name, filename)

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
    def _patch_htp_polling_config(data: dict, bundle_name: str, filename: str) -> None:
        """
        Patch QnnHtp backend config to set 'poll: false'.
        This prevents worker threads from busy-waiting and consuming 100% CPU while idle.
        """
        try:
            # Check all known root node keys
            for node_key in ("dialog", "text-generator", "text_generator", "textGenerator", "image-encoder", "imageEncoder", "lutEncoder", "text-encoder"):
                node = data.get(node_key)
                if isinstance(node, dict):
                    engine = node.get("engine")
                    if isinstance(engine, dict):
                        backend = engine.get("backend")
                        if isinstance(backend, dict):
                            # The key could be "QnnHtp" or "qnnHtp" depending on version
                            qnn_htp = backend.get("QnnHtp") or backend.get("qnnHtp")
                            if isinstance(qnn_htp, dict) and qnn_htp.get("poll") is True:
                                qnn_htp["poll"] = False
                                logger.info(
                                    f"[CPU OPTIMIZATION] Patched QnnHtp polling config from true to false "
                                    f"in {bundle_name}/{filename} ({node_key}) to reduce idle CPU usage."
                                )
        except Exception as e:
            logger.warning(f"Error patching HTP polling config in {bundle_name}/{filename}: {e}")

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
