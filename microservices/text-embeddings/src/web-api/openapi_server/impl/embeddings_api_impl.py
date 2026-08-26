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
import multiprocessing
import json

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


class SubprocessBackendWrapper(EmbeddingBackend):
    """
    Generic wrapper to run any backend inside a separate 'spawn' subprocess.
    """
    def __init__(self, backend_cls: type, model_path: str, model_id: str):
        self.backend_cls = backend_cls
        self.model_path = model_path
        self.model_id = model_id
        self.ctx = multiprocessing.get_context("spawn")
        self.req_queue = self.ctx.Queue()
        self.res_queue = self.ctx.Queue()
        self.proc = self.ctx.Process(target=self._worker_loop, args=(self.req_queue, self.res_queue))

        # Temporary bypass for Python's "daemonic processes are not allowed to have children" assertion.
        # FastAPI worker processes (Uvicorn/Hypercorn/Gunicorn) can be daemonic.
        current_proc = multiprocessing.current_process()
        is_daemon = getattr(current_proc, "daemon", False)
        if is_daemon:
            current_proc.daemon = False
        try:
            self.proc.start()
        finally:
            if is_daemon:
                current_proc.daemon = True

        # Wait for initialization status to propagate any errors cleanly
        res = self.res_queue.get()
        if res["status"] == "error":
            raise RuntimeError(f"Failed to initialize backend in subprocess: {res['error']}")

    def _worker_loop(self, req_queue: multiprocessing.Queue, res_queue: multiprocessing.Queue):
        try:
            # We initialize logging in the child process
            LoggerConfig.initialize()
            backend = self.backend_cls.from_model_path(self.model_path)
            res_queue.put({"status": "ok"})
        except Exception as e:
            res_queue.put({"status": "error", "error": str(e)})
            return

        while True:
            cmd = req_queue.get()
            op = cmd.get("op")
            if op == "close":
                try:
                    backend.close()
                except Exception:
                    pass
                break
            elif op == "embed":
                try:
                    res = backend.embed_texts(cmd["texts"])
                    res_queue.put({"status": "ok", "result": res})
                except Exception as e:
                    res_queue.put({"status": "error", "error": str(e)})
            elif op == "encode_tokens":
                try:
                    res = backend.encode_tokens(cmd["text"])
                    res_queue.put({"status": "ok", "result": res})
                except Exception as e:
                    res_queue.put({"status": "error", "error": str(e)})

    def _check_alive(self):
        if not self.proc or not self.proc.is_alive():
            raise RuntimeError("Backend subprocess died unexpectedly.")

    def embed_texts(self, texts: List[str]) -> List[List[float]]:
        self._check_alive()
        self.req_queue.put({"op": "embed", "texts": texts})
        res = self.res_queue.get()
        if res["status"] == "error":
            raise RuntimeError(res["error"])
        return res["result"]

    def encode_tokens(self, text: str) -> List[int]:
        self._check_alive()
        self.req_queue.put({"op": "encode_tokens", "text": text})
        res = self.res_queue.get()
        if res["status"] == "error":
            raise RuntimeError(res["error"])
        return res["result"]

    def close(self) -> None:
        if self.proc and self.proc.is_alive():
            try:
                self.req_queue.put({"op": "close"})
                self.proc.join(timeout=2)
            except Exception:
                pass
            if self.proc.is_alive():
                self.proc.terminate()
                self.proc.join()
        self.proc = None

    @property
    def backend_lib(self) -> str:
        return getattr(self.backend_cls, "backend_lib", getattr(self.backend_cls, "snpe_lib", "N/A"))


def _backend_factory(model_path: str, model_id: str = "") -> EmbeddingBackend:
    """
    Instantiate the correct :class:`EmbeddingBackend` for *model_path*.

    Selection is primarily based on the companion metadata.json runtime,
    explicit requested model_id, and file extension:

    * ``.tflite``  → :class:`NomicEmbedBackend` (LiteRT / TFLite path)
    * ``.bin``     → :class:`SimpleQnnEmbeddingApp` (QNN binary path)
    * ``.dlc``     → :class:`SimpleSnpeEmbeddingApp` or :class:`SimpleQnnEmbeddingApp` (depending on metadata.json)

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

    # 1. Discover runtime and correct file name from metadata.json next to the model file (Primary decision maker)
    runtime = None
    model_dir = os.path.dirname(model_path)
    metadata_path = os.path.join(model_dir, "metadata.json")
    if os.path.isfile(metadata_path):
        try:
            with open(metadata_path, encoding="utf-8") as f:
                meta = json.load(f)
                runtime = meta.get("runtime", "").lower()
                logger.info(f"Discovered runtime {runtime!r} from metadata.json: {metadata_path}")

                # Automatically resolve the correct model file from metadata.json model_files dictionary
                model_files = meta.get("model_files", {})
                if model_files:
                    first_file = list(model_files.keys())[0]
                    correct_path = os.path.join(model_dir, first_file)
                    if os.path.isfile(correct_path) and correct_path != model_path:
                        logger.info(f"Updating model_path to correct file from metadata.json: {correct_path!r}")
                        model_path = correct_path
                        _, ext = os.path.splitext(model_path)
                        ext = ext.lower()
        except Exception as e:
            logger.warning(f"Failed to read metadata.json at {metadata_path}: {e}")

    # Fallback checking: If file does not exist, speculative re-mapping
    if not os.path.exists(model_path):
        alt_path = None
        if ext == ".bin":
            alt_path = model_path.replace(".bin", ".dlc")
        elif ext == ".dlc":
            alt_path = model_path.replace(".dlc", ".bin")
        if alt_path and os.path.exists(alt_path):
            logger.info(f"Requested path {model_path!r} does not exist, but found alternative path: {alt_path!r}. Switching path.")
            model_path = alt_path
            _, ext = os.path.splitext(model_path)
            ext = ext.lower()

    # 2. Determine backend class using metadata.json runtime first
    backend_cls = None
    if runtime is not None:
        if runtime in ("qnn_dlc", "qnn", "qnn_context_binary"):
            backend_cls = SimpleQnnEmbeddingApp
        elif runtime == "snpe":
            backend_cls = SimpleSnpeEmbeddingApp
        elif runtime in ("litert", "tflite"):
            backend_cls = NomicEmbedBackend

    # 3. Fallback to model_id explicit requests if not guided by metadata.json runtime
    if backend_cls is None:
        if "snpe" in model_id.lower():
            backend_cls = SimpleSnpeEmbeddingApp
        elif "qnn" in model_id.lower():
            backend_cls = SimpleQnnEmbeddingApp
        elif "tflite" in model_id.lower() or "litert" in model_id.lower():
            backend_cls = NomicEmbedBackend

    # 4. Fallback to extension-based defaults if both metadata/model_id are not available
    if backend_cls is None:
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
        f"for model {model_path!r} (model_id: {model_id!r}, runtime: {runtime!r})"
    )
    return SubprocessBackendWrapper(backend_cls, model_path, model_id)


# ---------------------------------------------------------------------------
# Object-Oriented Backend Lifecycle Manager (Replaces Global Cache Variables)
# ---------------------------------------------------------------------------
# Backend initialisation is expensive (DMA buffers, QNN/SNPE engines, model graph compiles).
# However, Qualcomm's CDSP FastRPC hardware driver context cannot simultaneously host
# conflicting domains (e.g. SNPE vs QNN/LiteRT) within the same process space.
#
# BackendManager is a clean object-oriented singleton that enforces AT MOST ONE
# active backend context in memory, completely eliminating FastRPC conflicts
# and ensuring hardware resources are warm.
# ---------------------------------------------------------------------------

class BackendManager:
    """
    Singleton manager for controlling the lifecycle of active embedding backends.
    Ensures that at most one backend is active at any time to prevent hardware/Ion/CDSP conflicts.
    """
    _instance: Optional[BackendManager] = None
    _active_backend: Optional[EmbeddingBackend] = None
    _active_key: Optional[str] = None
    _lock = threading.Lock()
    _inference_lock = threading.Lock()
    _reload_count = 0

    def __new__(cls):
        if cls._instance is None:
            cls._instance = super(BackendManager, cls).__new__(cls)
        return cls._instance

    @classmethod
    def get_backend(cls, model_path: str, model_id: str) -> EmbeddingBackend:
        cache_key = f"{model_path}:{model_id}"
        with cls._lock:
            # Check for config reload signals
            current_reload = model_config_manager.get_reload_count()
            if current_reload != cls._reload_count:
                logger.info(f"ModelConfigManager reload detected ({cls._reload_count} -> {current_reload}). Clearing active backend.")
                cls._clear_locked()
                cls._reload_count = current_reload

            # If the active backend is different, close it first before loading the new one
            if cls._active_key != cache_key:
                if cls._active_backend is not None:
                    logger.info(f"Switching backend from {cls._active_key!r} to {cache_key!r}. Releasing hardware resources.")
                    cls._clear_locked()

                logger.info(f"Initialising backend: {cache_key!r}")
                cls._active_backend = _backend_factory(model_path, model_id)
                cls._active_key = cache_key
                logger.info(f"Backend {cls._active_backend.__class__.__name__!r} is ready.")

            return cls._active_backend

    @classmethod
    def get_inference_lock(cls) -> threading.Lock:
        return cls._inference_lock

    @classmethod
    def _clear_locked(cls):
        if cls._active_backend is not None:
            logger.info(f"Closing and releasing resources for active backend key={cls._active_key!r}")
            try:
                cls._active_backend.close()
            except Exception as e:
                logger.warning(f"Error while closing backend key={cls._active_key!r}: {e}")
            cls._active_backend = None
            cls._active_key = None

    @classmethod
    def clear(cls):
        with cls._lock:
            cls._clear_locked()


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
            # Retrieve the backend from the BackendManager (handles switches and reload checks automatically)
            backend = BackendManager.get_backend(model, model_id)

            with BackendManager.get_inference_lock():
                # Log the active backend and its associated runtime library/context details
                active_lib = getattr(backend, "backend_lib", getattr(backend, "snpe_lib", "N/A"))
                logger.info(
                    f"Executing inference on model_id: {model_id!r} "
                    f"using backend class: {backend.__class__.__name__!r} "
                    f"and runtime library: {active_lib!r}"
                )

                # All backends implement EmbeddingBackend, so no branching is needed here.
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
