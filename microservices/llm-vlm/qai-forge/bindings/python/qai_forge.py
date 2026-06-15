# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# ─────────────────────────────────────────────────────────────────────────────
# qai_forge.py — Python bindings for qai-forge Layer 2
#
# Uses the built-in ctypes module — zero build dependencies.
# Loads libqai_forge.so at import time.
#
# Usage:
#   export QAI_FORGE_LIB=/path/to/libqai_forge.so   # or rely on LD_LIBRARY_PATH
#   from qai_forge import QaiForge
#
#   llm = QaiForge()
#   resp = llm.chat("Qwen3-1.7B", [{"role": "user", "content": "Hello"}])
#   print(resp["content"])
# ─────────────────────────────────────────────────────────────────────────────

import ctypes
import json
import os
import queue
import threading
from typing import Dict, Any, Iterator, List, Optional

# ── Load shared library ────────────────────────────────────────────────────────
_lib_path = os.environ.get("QAI_FORGE_LIB", "libqai_forge.so")
try:
    _lib = ctypes.CDLL(_lib_path)
except OSError as e:
    raise ImportError(
        f"Failed to load {_lib_path}. "
        "Set QAI_FORGE_LIB=/path/to/libqai_forge.so or add its directory to LD_LIBRARY_PATH."
    ) from e

# ── C function signatures ──────────────────────────────────────────────────────
_lib.qai_forge_chat_blocking.argtypes = [
    ctypes.c_char_p,                    # request_json
    ctypes.POINTER(ctypes.c_char_p),    # response_json_out
    ctypes.POINTER(ctypes.c_char_p),    # error_out
]
_lib.qai_forge_chat_blocking.restype = ctypes.c_int

# Streaming callback: void (*)(const char* chunk_json, void* user_data)
_STREAM_CB_TYPE = ctypes.CFUNCTYPE(None, ctypes.c_char_p, ctypes.c_void_p)

_lib.qai_forge_chat_streaming.argtypes = [
    ctypes.c_char_p,                    # request_json
    _STREAM_CB_TYPE,                    # callback
    ctypes.c_void_p,                    # user_data
    ctypes.POINTER(ctypes.c_char_p),    # error_out
]
_lib.qai_forge_chat_streaming.restype = ctypes.c_int

_lib.qai_forge_free_string.argtypes = [ctypes.c_char_p]
_lib.qai_forge_free_string.restype = None

_lib.qai_forge_version.argtypes = []
_lib.qai_forge_version.restype = ctypes.c_char_p


# ── Public exceptions ──────────────────────────────────────────────────────────
class QaiForgeError(Exception):
    """Raised when the C API returns a non-zero error code."""
    pass


# ── Public API ─────────────────────────────────────────────────────────────────
class QaiForge:
    """
    Pythonic wrapper around the qai-forge Layer 2 C API.

    Calls ChatOrchestrator directly in-process — no HTTP/gRPC/D-Bus server needed.

    Examples
    --------
    Blocking completion::

        llm = QaiForge()
        resp = llm.chat("Qwen3-1.7B", [{"role": "user", "content": "Hello"}])
        print(resp["content"])

    Streaming completion::

        for chunk in llm.chat_stream("Qwen3-1.7B", [{"role": "user", "content": "Hello"}]):
            print(chunk.get("content_delta", ""), end="", flush=True)

    Multi-turn session (pass the same ``user`` value across turns)::

        resp1 = llm.chat("Qwen3-1.7B",
                          [{"role": "user", "content": "My name is Alice"}],
                          user="session-alice")
        resp2 = llm.chat("Qwen3-1.7B",
                          [{"role": "user", "content": "What is my name?"}],
                          user="session-alice")
        print(resp2["content"])  # → "Your name is Alice."
    """

    @staticmethod
    def version() -> str:
        """Return the library version string."""
        return _lib.qai_forge_version().decode("utf-8")

    def chat(
        self,
        model: str,
        messages: List[Dict[str, str]],
        *,
        max_tokens: int = 1024,
        temperature: float = 1.0,
        top_p: float = 1.0,
        top_k: int = 40,
        user: Optional[str] = None,
    ) -> Dict[str, Any]:
        """
        Blocking chat completion.

        Parameters
        ----------
        model : str
            Model identifier (e.g. ``"Qwen3-1.7B"``).
        messages : list of dict
            OpenAI-style message list: ``[{"role": "user", "content": "..."}]``.
        max_tokens : int
            Maximum number of completion tokens.
        temperature : float
            Sampling temperature (0.0–2.0).
        top_p : float
            Top-p nucleus sampling.
        top_k : int
            Top-k sampling.
        user : str, optional
            Session ID for multi-turn conversations.

        Returns
        -------
        dict
            Response with keys: ``id``, ``model``, ``content``,
            ``reasoning_content`` (thinking models only), ``finish_reason``,
            ``prompt_tokens``, ``completion_tokens``, ``total_tokens``.

        Raises
        ------
        QaiForgeError
            If the C API returns a non-zero error code.
        """
        req_bytes = self._build_request_bytes(
            model, messages, max_tokens, temperature, top_p, top_k, user
        )
        resp_ptr = ctypes.c_char_p(None)
        err_ptr  = ctypes.c_char_p(None)

        rc = _lib.qai_forge_chat_blocking(
            req_bytes,
            ctypes.byref(resp_ptr),
            ctypes.byref(err_ptr),
        )

        if rc != 0:
            err = err_ptr.value.decode("utf-8") if err_ptr.value else "Unknown error"
            if err_ptr.value:
                _lib.qai_forge_free_string(err_ptr)
            raise QaiForgeError(f"[{rc}] {err}")

        result = json.loads(resp_ptr.value.decode("utf-8"))
        _lib.qai_forge_free_string(resp_ptr)
        return result

    def chat_stream(
        self,
        model: str,
        messages: List[Dict[str, str]],
        *,
        max_tokens: int = 1024,
        temperature: float = 1.0,
        top_p: float = 1.0,
        top_k: int = 40,
        user: Optional[str] = None,
    ) -> Iterator[Dict[str, Any]]:
        """
        Streaming chat completion.

        Yields chunk dicts as tokens arrive from the model. Each chunk has:
        - ``content_delta`` (str): the token text (intermediate chunks)
        - ``reasoning_delta`` (str): thinking content (reasoning models only)
        - ``finish_reason`` (str): non-empty on the final chunk (``"stop"``, ``"length"``)
        - ``role`` (str): ``"assistant"`` on the first chunk only

        The C API is synchronous, so inference runs in a background thread.
        Tokens are passed to the main thread via a ``queue.Queue``.

        Parameters
        ----------
        model, messages, max_tokens, temperature, top_p, top_k, user
            Same as :meth:`chat`.

        Yields
        ------
        dict
            One dict per token.

        Raises
        ------
        QaiForgeError
            If the C API returns a non-zero error code.
        """
        req_bytes = self._build_request_bytes(
            model, messages, max_tokens, temperature, top_p, top_k, user
        )
        q: "queue.Queue[Any]" = queue.Queue()
        _SENTINEL = object()

        def _on_chunk(chunk_json_bytes: bytes, _user_data: Any) -> None:
            # Called from the C++ thread — must not block
            q.put(json.loads(chunk_json_bytes.decode("utf-8")))

        def _run() -> None:
            err_ptr = ctypes.c_char_p(None)
            # Keep a reference to the callback so it isn't GC'd during the call
            cb = _STREAM_CB_TYPE(_on_chunk)
            rc = _lib.qai_forge_chat_streaming(
                req_bytes, cb, None, ctypes.byref(err_ptr)
            )
            if rc != 0:
                err = err_ptr.value.decode("utf-8") if err_ptr.value else "Unknown"
                if err_ptr.value:
                    _lib.qai_forge_free_string(err_ptr)
                q.put(QaiForgeError(f"[{rc}] {err}"))
            q.put(_SENTINEL)

        t = threading.Thread(target=_run, daemon=True)
        t.start()

        while True:
            item = q.get()
            if item is _SENTINEL:
                break
            if isinstance(item, QaiForgeError):
                t.join()
                raise item
            yield item

        t.join()

    # ── Internal helpers ───────────────────────────────────────────────────────

    @staticmethod
    def _build_request_bytes(
        model: str,
        messages: List[Dict[str, str]],
        max_tokens: int,
        temperature: float,
        top_p: float,
        top_k: int,
        user: Optional[str],
    ) -> bytes:
        req: Dict[str, Any] = {
            "model":                 model,
            "messages":              messages,
            "max_completion_tokens": max_tokens,
            "temperature":           temperature,
            "top_p":                 top_p,
            "top_k":                 top_k,
        }
        if user:
            req["user"] = user
        return json.dumps(req).encode("utf-8")
