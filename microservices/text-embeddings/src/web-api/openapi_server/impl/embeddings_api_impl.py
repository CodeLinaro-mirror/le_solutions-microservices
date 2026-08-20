# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from __future__ import annotations

import os
import threading
import time
from typing import Dict, List, Optional, Tuple, Union
from fastapi import HTTPException
import asyncio
import base64

try:
    import numpy as np
except Exception:
    np = None

from openapi_server.apis.embeddings_api_base import BaseEmbeddingsApi
from openapi_server.models.create_embeddings_request import CreateEmbeddingsRequest
from openapi_server.models.create_embeddings_response import CreateEmbeddingsResponse
from openapi_server.models.embedding import Embedding
from openapi_server.models.embedding_usage import EmbeddingUsage
from openapi_server.logger.logger_config import LoggerConfig
from openapi_server.impl.constant import HttpStatusCodes, ErrorMessages
from openapi_server.managers.model_config_manager import model_config_manager

from openapi_server.impl.embedding_backend import EmbeddingBackend
from openapi_server.impl.litert_backend.backend import (
    NomicEmbedBackend,
    normalize_encoding_format,
)
from openapi_server.impl.text_embed_qnn import SimpleQnnEmbeddingApp
from openapi_server.impl.snpe_backend.backend import SimpleSnpeEmbeddingApp

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)

# ---------------------------------------------------------------------------
# Supported file-extension → backend mapping
# ---------------------------------------------------------------------------
# .tflite  →  NomicEmbedBackend      (LiteRT / TFLite compiled model)
# .bin     →  SimpleQnnEmbeddingApp  (QNN pre-compiled context binary)
# .dlc     →  SimpleSnpeEmbeddingApp (SNPE Deep Learning Container model)
#
# All classes implement EmbeddingBackend, so all downstream code in
# _get_embeddings_from_component is backend-agnostic.
# ---------------------------------------------------------------------------

_EXTENSION_TO_BACKEND: Dict[str, type] = {
    ".tflite": NomicEmbedBackend,
    ".bin":    SimpleQnnEmbeddingApp,
    ".dlc":    SimpleSnpeEmbeddingApp,
}


def _backend_factory(model_path: str, model_id: str = "") -> EmbeddingBackend:
    """
    Instantiate the correct :class:`EmbeddingBackend` for *model_path*.

    Selection is based on the requested model_id and file extension:

    * ``.tflite``  → :class:`NomicEmbedBackend` (LiteRT / TFLite path)
    * ``.bin``     → :class:`SimpleQnnEmbeddingApp` (QNN binary path)
    * ``.dlc`` (SNPE) → :class:`SimpleSnpeEmbeddingApp` (SNPE binary path)
    * ``.dlc`` (QNN) → :class:`SimpleQnnEmbeddingApp` (QNN context binary inside DLC)

    Parameters
    ----------
    model_path:
        Absolute or relative path to the model file.
    model_id:
        The requested model ID/key to guide backend selection.

    Returns
    -------
    EmbeddingBackend
        A fully initialised backend ready for inference.

    Raises
    ------
    ValueError
        If the file extension is not recognised.
    """
    _, ext = os.path.splitext(model_path)
    ext = ext.lower()

    if ext == ".dlc":
        # Force SNPE backend if explicitly requested
        if "snpe" in model_id.lower():
            logger.info(f"Explicit SNPE request for {model_path!r}. Selecting backend {SimpleSnpeEmbeddingApp.__name__!r}.")
            return SimpleSnpeEmbeddingApp.from_model_path(model_path)

        # Force QNN backend if explicitly requested
        if "qnn" in model_id.lower():
            logger.info(f"Explicit QNN request for {model_path!r}. Selecting backend {SimpleQnnEmbeddingApp.__name__!r}.")
            return SimpleQnnEmbeddingApp.from_model_path(model_path)

    backend_cls = _EXTENSION_TO_BACKEND.get(ext)
    if backend_cls is None:
        supported = ", ".join(sorted(_EXTENSION_TO_BACKEND))
        raise ValueError(
            f"Unrecognised model file extension '{ext}' for path '{model_path}'. "
            f"Supported extensions: {supported}. "
            "Use a .tflite file for the LiteRT/TFLite backend, a .bin file "
            "for the QNN pre-compiled context binary backend, or a .dlc file "
            "for the SNPE Deep Learning Container backend."
        )

    logger.info(
        f"Selecting backend {backend_cls.__name__!r} "
        f"for extension '{ext}' (model: {model_path!r})"
    )
    return backend_cls.from_model_path(model_path)


# ---------------------------------------------------------------------------
# Per-model backend cache
# ---------------------------------------------------------------------------
# Backend initialisation is expensive:
#   - NomicEmbedBackend:      loads libLiteRt.so, compiles the TFLite graph,
#                             allocates DMA tensor buffers.
#   - SimpleQnnEmbeddingApp:  loads libQnnHtp.so + libQnnSystem.so, creates
#                             QNN backend/context, deserialises the graph.
#   - SimpleSnpeEmbeddingApp: loads libSNPE.so, builds SNPE engine from DLC container.
#
# Re-creating the backend on every request adds 200–800 ms of cold-start
# latency and prevents the HTP hardware from staying warm.
#
# We keep one EmbeddingBackend instance per model path/key and serialise
# concurrent inference calls with a lock so that tensor buffers are never
# accessed from two threads simultaneously.
# ---------------------------------------------------------------------------

_backend_cache: Dict[str, EmbeddingBackend] = {}
_backend_locks: Dict[str, threading.Lock] = {}
_backend_meta_lock = threading.Lock()
_last_seen_reload_count = 0


def _clear_backend_cache() -> None:
    """
    Safely close and release hardware resources of all cached backends,
    then clear the cache structures.
    """
    global _backend_cache, _backend_locks
    with _backend_meta_lock:
        logger.info(f"Clearing model backend cache. Releasing {len(_backend_cache)} cached backend(s).")
        for cache_key, backend in list(_backend_cache.items()):
            try:
                logger.info(f"Closing and releasing resources for cached backend key={cache_key!r}")
                backend.close()
            except Exception as e:
                logger.warning(f"Error while closing backend key={cache_key!r}: {e}")
        _backend_cache.clear()
        _backend_locks.clear()
        logger.info("Model backend cache cleared successfully.")


def _get_backend(model_path: str, model_id: str) -> Tuple[EmbeddingBackend, threading.Lock]:
    """
    Return ``(backend, lock)`` for *model_path* and *model_id*, creating them on first call.

    Thread-safe: concurrent callers for the same backend will block
    until the backend is ready, then all receive the same cached instance.

    Parameters
    ----------
    model_path:
        Absolute path to the model file.
    model_id:
        The requested model ID/key to guide backend selection.

    Returns
    -------
    Tuple[EmbeddingBackend, threading.Lock]
        The cached backend instance and its associated inference lock.
    """

    cache_key = f"{model_path}:{model_id}"
    # Phase 1: obtain (or create) the per-model lock without holding the
    # global meta-lock for longer than necessary.
    with _backend_meta_lock:
        if cache_key not in _backend_locks:
            _backend_locks[cache_key] = threading.Lock()
        lock = _backend_locks[cache_key]

    # Phase 2: create the backend if it does not exist yet.  The per-model
    # lock serialises concurrent first-time initialisations for the same path.
    with lock:
        if cache_key not in _backend_cache:
            logger.info(f"Initialising backend for model: {model_path!r} (key: {model_id!r})")
            _backend_cache[cache_key] = _backend_factory(model_path, model_id)
            logger.info(
                f"Backend {_backend_cache[cache_key].__class__.__name__!r} "
                f"ready for model: {model_path!r}"
            )
        return _backend_cache[cache_key], lock


# ---------------------------------------------------------------------------
# API implementation
# ---------------------------------------------------------------------------

class EmbeddingsApiImpl(BaseEmbeddingsApi):
    async def create_embeddings(
        self,
        create_embeddings_request: CreateEmbeddingsRequest,
    ) -> CreateEmbeddingsResponse:
        """
        Create embeddings endpoint.

        Args:
            create_embeddings_request: Request containing model, input text,
                and optional parameters.

        Returns:
            CreateEmbeddingsResponse: Response with embeddings data.
        """
        try:
            # ── Validate model ────────────────────────────────────────────
            requested_model = create_embeddings_request.model
            if not requested_model or requested_model == "None" or requested_model == "":
                requested_model = model_config_manager.get_default_model()

            if not model_config_manager.is_model_available(requested_model):
                available_models = model_config_manager.get_available_model_ids()
                logger.error(f"Unsupported model requested: {requested_model}")
                raise HTTPException(
                    status_code=HttpStatusCodes.BAD_REQUEST,
                    detail=(
                        f"Model '{requested_model}' is not supported. "
                        f"Available models: {', '.join(available_models)}"
                    ),
                )

            logger.info(f"Creating embeddings for model: {requested_model}")

            # ── Normalise inputs ──────────────────────────────────────────
            input_data = create_embeddings_request.input

            if isinstance(input_data.actual_instance, str):
                inputs = [input_data.actual_instance]
            elif isinstance(input_data.actual_instance, list):
                inputs = input_data.actual_instance
            else:
                inputs = [str(input_data.actual_instance)]

            logger.info(f"Processing {len(inputs)} input(s)")

            # ── Resolve model binary path ─────────────────────────────────
            model_info = model_config_manager.get_model_info(requested_model)
            model_file = model_info["model_file"]
            models_path = os.getenv("T2E_MODEL_DIR", "/mnt/work/models")

            # Dynamically search for the model file in subdirectories if not at the root
            model_binary_path = os.path.join(models_path, model_file)
            if not os.path.exists(model_binary_path):
                model_basename = os.path.basename(model_file)
                try:
                    found_path = None
                    for root, dirs, files in os.walk(models_path):
                        depth = root[len(models_path):].count(os.sep)
                        if depth > 3:  # scan up to 3 levels deep
                            dirs.clear()
                            continue
                        if model_basename in files:
                            found_path = os.path.join(root, model_basename)
                            break
                    if found_path:
                        logger.info(f"Dynamically discovered model path: {found_path}")
                        model_binary_path = found_path
                except Exception as e:
                    logger.warning(f"Error dynamically searching for model {model_file}: {e}")

            # ── Run inference ─────────────────────────────────────────────
            embeddings_data, token_counts = await self._get_embeddings_from_component(
                inputs=inputs,
                model=model_binary_path,
                model_id=requested_model,
                dimensions=create_embeddings_request.dimensions,
                encoding_format=create_embeddings_request.encoding_format,
            )

            # ── Build response ────────────────────────────────────────────
            embedding_objects = [
                Embedding(
                    object="embedding",
                    embedding=emb_vector,
                    index=idx,
                )
                for idx, emb_vector in enumerate(embeddings_data)
            ]

            total_tokens = sum(token_counts)
            usage = EmbeddingUsage(
                prompt_tokens=total_tokens,
                total_tokens=total_tokens,
            )

            response = CreateEmbeddingsResponse(
                object="list",
                data=embedding_objects,
                model=create_embeddings_request.model,
                usage=usage,
            )

            logger.info(f"Successfully created {len(embedding_objects)} embedding(s)")
            return response

        except HTTPException:
            raise
        except ValueError as e:
            # Unrecognised file extension or other configuration error
            logger.error(f"Backend selection error: {e}")
            raise HTTPException(
                status_code=HttpStatusCodes.BAD_REQUEST,
                detail=str(e),
            )
        except Exception as e:
            logger.error(f"Unexpected error in create_embeddings: {e}")
            raise HTTPException(
                status_code=HttpStatusCodes.INTERNAL_SERVER_ERROR,
                detail=ErrorMessages.UNEXPECTED_ERROR,
            )

    async def _get_embeddings_from_component(
        self,
        inputs: List[str],
        model: str,
        model_id: str = "",
        dimensions: Optional[int] = None,
        encoding_format: Optional[str] = "float",
    ) -> Tuple[List[Union[List[float], str]], List[int]]:
        """
        Run inference using the backend selected by *model*'s file extension.

        The backend is chosen once (on first call for a given *model* path)
        and cached for the lifetime of the process.  Concurrent requests for
        the same model are serialised by a per-model lock.

        Parameters
        ----------
        inputs:
            List of input strings to embed.
        model:
            Absolute path to the model file.  Extension determines backend:
            ``.tflite`` → NomicEmbedBackend, ``.bin`` → SimpleQnnEmbeddingApp,
            ``.dlc`` → SimpleSnpeEmbeddingApp.
        model_id:
            The requested model ID/key to guide backend selection.
        dimensions:
            If set, truncate each embedding vector to this many dimensions.
        encoding_format:
            ``"float"`` (default) or ``"base64"``.

        Returns
        -------
        Tuple[List[Union[List[float], str]], List[int]]
            ``(embeddings, token_counts)`` where *embeddings* is a list of
            float vectors (or base64 strings) and *token_counts* is the
            number of tokens in each input string.
        """
        ef = normalize_encoding_format(encoding_format)

        def _work() -> Tuple[List[Union[List[float], str]], List[int]]:
            global _last_seen_reload_count
            current_reload = model_config_manager.get_reload_count()
            if current_reload != _last_seen_reload_count:
                logger.info(f"ModelConfigManager reload detected ({_last_seen_reload_count} -> {current_reload}). Clearing backend cache and releasing hardware resources.")
                _clear_backend_cache()
                _last_seen_reload_count = current_reload

            # Retrieve (or create) the cached backend and its inference lock.
            # The lock ensures tensor buffers are never accessed concurrently.
            backend, lock = _get_backend(model, model_id)

            with lock:
                # Log the active backend and its associated runtime library/context details
                active_lib = getattr(backend, "backend_lib", "N/A")
                logger.info(
                    f"Executing inference on model_id: {model_id!r} "
                    f"using backend class: {backend.__class__.__name__!r} "
                    f"and runtime library: {active_lib!r}"
                )

                # Both NomicEmbedBackend and SimpleQnnEmbeddingApp implement
                # EmbeddingBackend, so no branching is needed here.
                t_inf0 = time.time()
                try:
                    vectors: List[List[float]] = backend.embed_texts(inputs)
                    token_counts: List[int] = backend.count_tokens(inputs)
                    logger.info(f"Inference execution for model_id {model_id!r} succeeded in {time.time() - t_inf0:.4f}s")
                except Exception as e:
                    logger.error(f"Inference execution for model_id {model_id!r} failed: {e}", exc_info=True)
                    raise

            # ── Optional dimension truncation ─────────────────────────────
            if dimensions and dimensions > 0:
                vectors = [v[:dimensions] for v in vectors]

            # ── Encode output ─────────────────────────────────────────────
            if ef == "float":
                return vectors, token_counts

            # base64 encoding path
            encoded: List[str] = []
            for v in vectors:
                if np is not None:
                    raw = np.asarray(v, dtype=np.float32).tobytes()
                else:
                    import struct
                    raw = struct.pack("<" + "f" * len(v), *v)
                encoded.append(base64.b64encode(raw).decode("ascii"))

            return encoded, token_counts

        return await asyncio.to_thread(_work)
