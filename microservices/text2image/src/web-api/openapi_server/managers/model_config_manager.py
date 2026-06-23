# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import json
import os
import re
from typing import Any, Dict, List, Optional

from openapi_server.logger.logger_config import LoggerConfig

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)

# Output tensor names used to identify each model component inside a bundle
_TEXT_ENCODER_OUTPUT = "text_embedding"
_UNET_OUTPUT = "output_latent"
_VAE_OUTPUT = "image"

# Regex that converts an aihub model_id to the normalised API model ID.
# Examples:
#   "stable_diffusion_v2_1" → "stable-diffusion-2.1"
#   "stable_diffusion_v1_5" → "stable-diffusion-1.5"
_MODEL_ID_RE = re.compile(r"stable_diffusion_v(\d+)_(\d+)")


def _normalize_model_id(raw_id: str) -> str:
    """
    Convert an aihub ``model_id`` string to the normalised API model ID.

    Parameters
    ----------
    raw_id : str
        Raw model ID from ``metadata.json``, e.g. ``"stable_diffusion_v2_1"``.

    Returns
    -------
    str
        Normalised API model ID, e.g. ``"stable-diffusion-2.1"``.
        Returns *raw_id* unchanged if the pattern is not recognised.
    """
    match = _MODEL_ID_RE.search(raw_id)
    if match:
        return f"stable-diffusion-{match.group(1)}.{match.group(2)}"
    # Fallback: replace underscores with dashes
    return raw_id.replace("_", "-")


def _parse_metadata(meta: Dict[str, Any], bundle_dir: str) -> Optional[Dict[str, Any]]:
    """
    Extract a model registry entry from a parsed ``metadata.json`` dict.

    The function identifies each model component (text encoder, UNet, VAE)
    by inspecting the output tensor names declared in the metadata:

    * ``"text_embedding"``  → text encoder
    * ``"output_latent"``   → UNet
    * ``"image"``           → VAE decoder

    The text-embedding output shape ``[1, seq_len, hidden_size]`` is used to
    derive ``hidden_size`` (768 for SD 1.5, 1024 for SD 2.1).

    Parameters
    ----------
    meta : dict
        Parsed contents of ``metadata.json``.
    bundle_dir : str
        Absolute path to the directory that contains ``metadata.json`` and
        the model binary files.

    Returns
    -------
    dict or None
        Registry entry dict, or ``None`` if the metadata is incomplete /
        unrecognised.
    """
    raw_id = meta.get("model_id", "")
    if not raw_id:
        logger.warning(f"Bundle at '{bundle_dir}' has no 'model_id' in metadata – skipping.")
        return None

    model_id = _normalize_model_id(raw_id)
    display_name = meta.get("model_name", model_id)

    model_files: Dict[str, str] = {}
    hidden_size: Optional[int] = None

    for filename, spec in meta.get("model_files", {}).items():
        outputs = spec.get("outputs", {})

        if _TEXT_ENCODER_OUTPUT in outputs:
            model_files["text_encoder"] = filename
            shape = outputs[_TEXT_ENCODER_OUTPUT].get("shape", [])
            # shape is [batch, seq_len, hidden_size]
            if len(shape) >= 3:
                hidden_size = int(shape[2])

        elif _UNET_OUTPUT in outputs:
            model_files["unet"] = filename

        elif _VAE_OUTPUT in outputs:
            model_files["vae"] = filename

    required = {"text_encoder", "unet", "vae"}
    missing = required - model_files.keys()
    if missing:
        logger.warning(
            f"Bundle at '{bundle_dir}' is missing model components {missing} – skipping."
        )
        return None

    entry: Dict[str, Any] = {
        "display_name": display_name,
        "bundle_dir": bundle_dir,
        "model_files": model_files,
        "max_tokens": 77,
    }
    if hidden_size is not None:
        entry["hidden_size"] = hidden_size

    logger.info(
        f"Discovered model '{model_id}' (hidden_size={hidden_size}) "
        f"from bundle '{os.path.basename(bundle_dir)}'"
    )
    return entry


class ModelConfigManager:
    """
    Singleton manager that discovers and caches available Stable Diffusion
    models by scanning the mounted models directory for aihub bundle
    subdirectories.

    An *aihub bundle* is a directory that contains a ``metadata.json`` file
    (as distributed by Qualcomm AI Hub) alongside the QNN context binary
    files.  Example layout::

        /mnt/work/models/
        └── stable_diffusion_v2_1-qnn_context_binary-w8a16-qualcomm_qcs9075/
            ├── metadata.json
            ├── text_encoder.bin
            ├── unet.bin
            └── vae.bin

    The manager scans ``T2I_MODEL_DIR`` (default ``/mnt/work/models``) on
    first use and caches the result.  Call :meth:`refresh` to re-scan after
    new bundles are added at runtime.

    The ``/models`` API endpoint and the image-generation pipeline both use
    this manager as their single source of truth for available models.
    """

    _instance: Optional["ModelConfigManager"] = None
    _models: Optional[Dict[str, Any]] = None

    def __new__(cls) -> "ModelConfigManager":
        if cls._instance is None:
            cls._instance = super().__new__(cls)
        return cls._instance

    # ------------------------------------------------------------------
    # Discovery
    # ------------------------------------------------------------------

    def _discover_models(self) -> Dict[str, Any]:
        """
        Scan ``T2I_MODEL_DIR`` for aihub bundle subdirectories and return a
        dict of ``normalized_model_id → model_entry``.
        """
        models_dir = os.getenv("T2I_MODEL_DIR", "/mnt/work/models")
        logger.info(f"Scanning '{models_dir}' for aihub model bundles…")

        if not os.path.isdir(models_dir):
            logger.error(f"Models directory '{models_dir}' does not exist.")
            return {}

        discovered: Dict[str, Any] = {}

        try:
            entries = list(os.scandir(models_dir))
        except PermissionError as exc:
            logger.error(f"Cannot scan '{models_dir}': {exc}")
            return {}

        for entry in entries:
            if not entry.is_dir():
                continue

            metadata_path = os.path.join(entry.path, "metadata.json")
            if not os.path.isfile(metadata_path):
                continue

            try:
                with open(metadata_path, encoding="utf-8") as f:
                    meta = json.load(f)
            except (json.JSONDecodeError, OSError) as exc:
                logger.warning(f"Could not read '{metadata_path}': {exc} – skipping.")
                continue

            model_entry = _parse_metadata(meta, bundle_dir=entry.path)
            if model_entry is None:
                continue

            model_id = model_entry.pop("model_id", None) or _normalize_model_id(
                meta.get("model_id", "")
            )
            # Re-derive model_id since _parse_metadata doesn't include it in the entry
            model_id = _normalize_model_id(meta.get("model_id", ""))
            discovered[model_id] = model_entry

        if not discovered:
            logger.warning(
                f"No valid aihub bundles found in '{models_dir}'. "
                "Ensure each bundle subdirectory contains a metadata.json."
            )
        else:
            logger.info(f"Discovered {len(discovered)} model(s): {list(discovered.keys())}")

        return discovered

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def get_models(self, force_refresh: bool = False) -> Dict[str, Any]:
        """
        Return the dict of available models, discovering them if necessary.

        Parameters
        ----------
        force_refresh : bool
            If ``True``, re-scan the models directory even if the cache is
            already populated.

        Returns
        -------
        dict
            ``{normalized_model_id: model_entry}`` where each entry contains
            ``display_name``, ``bundle_dir``, ``model_files``, ``hidden_size``,
            and ``max_tokens``.
        """
        if force_refresh or self._models is None:
            self._models = self._discover_models()
        return self._models

    def refresh(self) -> Dict[str, Any]:
        """
        Force a re-scan of the models directory and return the updated registry.

        Returns
        -------
        dict
            Updated ``{normalized_model_id: model_entry}`` dict.
        """
        return self.get_models(force_refresh=True)

    def get_default_model(self) -> str:
        """
        Return the ID of the first discovered model.

        Returns
        -------
        str
            Normalised model ID (e.g. ``"stable-diffusion-2.1"``).

        Raises
        ------
        ValueError
            If no models are available.
        """
        models = self.get_models()
        if not models:
            raise ValueError(
                "No models available. Ensure T2I_MODEL_DIR contains at least one "
                "aihub bundle subdirectory with a metadata.json."
            )
        return next(iter(models))

    def get_model_info(self, model_id: str) -> Optional[Dict[str, Any]]:
        """
        Return the registry entry for *model_id*, or ``None`` if not found.

        Parameters
        ----------
        model_id : str
            Normalised model ID, e.g. ``"stable-diffusion-2.1"``.
        """
        return self.get_models().get(model_id)

    def is_model_available(self, model_id: str) -> bool:
        """Return ``True`` if *model_id* is in the discovered registry."""
        return model_id in self.get_models()

    def get_available_model_ids(self) -> List[str]:
        """Return a list of all discovered normalised model IDs."""
        return list(self.get_models().keys())

    def extract_variant_from_model_id(self, model_id: str) -> str:
        """
        Extract the variant string from a normalised model ID.

        Parameters
        ----------
        model_id : str
            Normalised model ID, e.g. ``"stable-diffusion-2.1"``.

        Returns
        -------
        str
            Variant string, e.g. ``"2.1"`` or ``"1.5"``.
        """
        parts = model_id.split("-")
        variant = parts[-1] if parts else "2.1"
        logger.debug(f"Extracted variant '{variant}' from model ID '{model_id}'")
        return variant


# Module-level singleton – import and use this directly.
model_config_manager = ModelConfigManager()
