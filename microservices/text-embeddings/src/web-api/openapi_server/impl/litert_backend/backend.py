# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from __future__ import annotations

import os
import sys
import time
import base64
import traceback
import threading
import subprocess
import uuid

from dataclasses import dataclass
from typing import List, Optional, Tuple, Dict, Any, Union

import numpy as np
from multiprocessing.connection import Listener, Client

from openapi_server.impl.litert_backend.litert_api import LiteRtInterpreter, QualcommOptions, HW_CPU
from openapi_server.impl.embedding_backend import EmbeddingBackend
from openapi_server.impl.litert_backend.logger import get_logger

_log = get_logger(__name__)


# =========================================
# BackendConfig + Tokenizers + Backend class
# =========================================

@dataclass(frozen=True)
class BackendConfig:
    model_path: str
    runtime_lib: str = "libLiteRt.so"
    dispatch_dir: Optional[str] = None
    seq_len: int = 128
    normalize: bool = True
    prefer_htp: bool = True
    require_htp: bool = True
    qcom_perf_mode: Optional[int] = None
    signature_index: int = 0
    output_dtype: str = "float32"
    embed_dim: Optional[int] = None

    tokenizer_dir: Optional[str] = os.environ.get("TOKENIZER_DIR")

    sentencepiece_model: Optional[str] = None


class Tokenizer:
    def encode_batch(self, texts: List[str], seq_len: int) -> Tuple[np.ndarray, np.ndarray]:
        raise NotImplementedError

    def encode(self, text: str) -> List[int]:
        """Return token IDs for a single text (no padding).  Used for token counting."""
        raise NotImplementedError


class HFTokenizer(Tokenizer):
    """
    Robust local HuggingFace tokenizer loader using tokenizers library.
    Works if tokenizer_dir contains tokenizer.json.
    """

    def __init__(self, tokenizer_dir: str):
        _log.info("HFTokenizer.__init__: initialising with tokenizer_dir=%r", tokenizer_dir)
        self.tokenizer_dir = tokenizer_dir
        self.tok = None

        # Load tokenizer.json directly
        tok_json = os.path.join(tokenizer_dir, "tokenizer.json")
        _log.debug("HFTokenizer.__init__: checking for direct tokenizer.json path: %r", tok_json)
        if os.path.exists(tok_json):
            from tokenizers import Tokenizer as HF_Tokenizer
            _log.debug("HFTokenizer.__init__: loading direct tokenizers.Tokenizer from file %r", tok_json)
            self.tok = HF_Tokenizer.from_file(tok_json)
            _log.info("HFTokenizer.__init__: successfully loaded direct tokenizers.Tokenizer")
            return

        raise ValueError(
            f"Tokenizer dir '{tokenizer_dir}' is not a valid HF tokenizer folder. "
            f"Expected tokenizer.json."
        )

    def encode_batch(self, texts: List[str], seq_len: int) -> Tuple[np.ndarray, np.ndarray]:
        _log.debug("HFTokenizer.encode_batch: encoding %d texts with seq_len=%d", len(texts), seq_len)
        try:
            vocab = self.tok.get_vocab()
            pad_id = vocab.get("[PAD]", vocab.get("<pad>", 0))

            self.tok.enable_padding(length=seq_len, pad_id=pad_id, pad_token="[PAD]")
            self.tok.enable_truncation(max_length=seq_len)

            encodings = self.tok.encode_batch(texts)

            input_ids_list = [enc.ids for enc in encodings]
            attention_mask_list = [enc.attention_mask for enc in encodings]

            input_ids = np.array(input_ids_list, dtype=np.int32)
            attn = np.array(attention_mask_list, dtype=np.int32)

            _log.debug("HFTokenizer.encode_batch: encoded input_ids shape=%s max_id=%d", input_ids.shape, int(input_ids.max()) if input_ids.size else -1)

            # Sanity check: huge ids usually mean mismatched tokenizer/vocab
            if input_ids.size and int(input_ids.max()) > 10_000_000:
                raise RuntimeError(
                    f"Suspicious token id max={int(input_ids.max())}. Tokenizer likely doesn't match model vocab."
                )
            return input_ids, attn
        except Exception as e:
            _log.error("HFTokenizer.encode_batch failed with exception: %s", e, exc_info=True)
            raise

    def encode(self, text: str) -> List[int]:
        """Tokenise a single string and return token IDs without padding."""
        _log.debug("HFTokenizer.encode: tokenising single string (len=%d) context=%r", len(text), text[:100])
        try:
            self.tok.no_padding()
            self.tok.no_truncation()
            enc = self.tok.encode(text)
            res = enc.ids
            _log.debug("HFTokenizer.encode: tokenised into %d IDs: %s", len(res), res[:10])
            return res
        except Exception as e:
            _log.error("HFTokenizer.encode failed with exception: %s", e, exc_info=True)
            raise


class SafePadTokenizer(Tokenizer):
    """Fallback to keep execution possible; NOT meaningful."""
    def encode_batch(self, texts: List[str], seq_len: int) -> Tuple[np.ndarray, np.ndarray]:
        B = len(texts)
        return np.zeros((B, seq_len), np.int32), np.zeros((B, seq_len), np.int32)

    def encode(self, text: str) -> List[int]:
        """SafePadTokenizer has no real vocabulary; returns an empty list."""
        return []


def find_tokenizer_dir(model_path: str) -> Optional[str]:
    # 1. Try to find dynamically near the model_path if provided
    if model_path:
        model_dir = os.path.dirname(model_path)
        if os.path.isdir(model_dir):
            json_path = os.path.join(model_dir, "tokenizer.json")
            if os.path.exists(json_path):
                _log.info(f"Dynamically discovered tokenizer.json near model: {json_path}")
                return model_dir
            # Recursively walk to find it
            try:
                for root, dirs, files in os.walk(model_dir):
                    depth = root[len(model_dir):].count(os.sep)
                    if depth > 2:
                        dirs.clear()
                        continue
                    if "tokenizer.json" in files:
                        found_json = os.path.join(root, "tokenizer.json")
                        _log.info(f"Dynamically discovered tokenizer.json in model subdirectory: {found_json}")
                        return root
            except Exception as e:
                _log.warning(f"Error while dynamically searching for tokenizer.json near {model_path}: {e}")

    # 2. Try to find dynamically in the models root directory
    models_dir = os.getenv("T2E_MODEL_DIR", "/mnt/work/models")
    if os.path.isdir(models_dir):
        try:
            for root, dirs, files in os.walk(models_dir):
                depth = root[len(models_dir):].count(os.sep)
                if depth > 3:
                    dirs.clear()
                    continue
                if "tokenizer.json" in files:
                    found_json = os.path.join(root, "tokenizer.json")
                    _log.info(f"Dynamically discovered tokenizer.json in T2E_MODEL_DIR: {found_json}")
                    return root
        except Exception as e:
            _log.warning(f"Error while dynamically searching for tokenizer.json in T2E_MODEL_DIR {models_dir}: {e}")

    # 3. Fallback to TOKENIZER_DIR
    tok_dir = os.getenv("TOKENIZER_DIR")
    if tok_dir and os.path.isdir(tok_dir):
        json_path = os.path.join(tok_dir, "tokenizer.json")
        if os.path.exists(json_path):
            return tok_dir

    return None


def build_tokenizer(cfg: BackendConfig) -> Tokenizer:
    tok_dir = find_tokenizer_dir(cfg.model_path)
    if tok_dir:
        return HFTokenizer(tok_dir)
    if cfg.tokenizer_dir and os.path.exists(os.path.join(cfg.tokenizer_dir, "tokenizer.json")):
        return HFTokenizer(cfg.tokenizer_dir)
    return SafePadTokenizer()


# ---------------------------------------------------------------------------
# NomicEmbedBackend
# ---------------------------------------------------------------------------

class NomicEmbedBackend(EmbeddingBackend):
    """
    LiteRT / TFLite embedding backend.

    Implements :class:`EmbeddingBackend` so it can be used interchangeably
    with :class:`~openapi_server.impl.text_embed_qnn.SimpleQnnEmbeddingApp`
    inside ``EmbeddingsApiImpl._get_embeddings_from_component``.

    Typical usage
    -------------
    ::

        backend = NomicEmbedBackend.from_model_path("/opt/embed_gen/model.tflite")
        vectors = backend.embed_texts(["hello world"])
        backend.close()
    """

    @classmethod
    def from_model_path(cls, model_path: str) -> NomicEmbedBackend:
        """
        Construct a ``NomicEmbedBackend`` from environment variables.

        Reads the same ``T2E_*`` variables that the subprocess worker uses so
        that HTP/NPU acceleration is honoured when the LiteRT path is chosen.
        """
        perf_mode_str = os.environ.get("T2E_PERF_MODE")
        embed_dim_str = os.environ.get("T2E_EMBED_DIM")

        # Automatically fallback to CPU (XNNPACK) for float models to avoid QNN desync/hang
        is_float = "float" in model_path.lower()
        prefer_htp_val = False if is_float else (os.environ.get("T2E_PREFER_HTP", "1") == "1")
        require_htp_val = False if is_float else (os.environ.get("T2E_REQUIRE_HTP", "1") == "1")

        if is_float:
            _log.info("Float model detected. Disabling HTP acceleration and falling back to CPU (XNNPACK) for stability.")

        cfg = BackendConfig(
            model_path=model_path,
            runtime_lib=os.environ.get("T2E_RUNTIME_LIB", "libLiteRt.so"),
            dispatch_dir=os.environ.get("T2E_DISPATCH_DIR") or None,
            seq_len=int(os.environ.get("T2E_SEQ_LEN", "128")),
            normalize=os.environ.get("T2E_NORMALIZE", "1") == "1",
            prefer_htp=prefer_htp_val,
            require_htp=require_htp_val,
            qcom_perf_mode=int(perf_mode_str) if perf_mode_str else None,
            signature_index=int(os.environ.get("T2E_SIGNATURE_INDEX", "0")),
            output_dtype=os.environ.get("T2E_DTYPE", "float32"),
            embed_dim=int(embed_dim_str) if embed_dim_str else None,
        )
        return cls(cfg)

    def __init__(self, cfg: BackendConfig, tokenizer: Optional[Tokenizer] = None):
        _log.info(
            "NomicEmbedBackend.__init__: model=%r prefer_htp=%s require_htp=%s "
            "seq_len=%d normalize=%s output_dtype=%r",
            cfg.model_path, cfg.prefer_htp, cfg.require_htp,
            cfg.seq_len, cfg.normalize, cfg.output_dtype,
        )
        self.cfg = cfg

        # ── Dispatch directory setup ──────────────────────────────────────
        # MUST be done before LiteRtCreateEnvironment (called inside
        # LiteRtInterpreter.__init__) so that the auto-registration path in
        # auto_registration.cc can locate the QNN HTP plugin.
        # Without this, the runtime logs:
        #   WARNING: [auto_registration.cc:71] NPU accelerator could not be
        #   loaded and registered: kLiteRtStatusErrorInvalidArgument
        # and silently falls back to XNNPACK CPU.
        # Default to /usr/lib where the Dockerfile stages our custom compiled
        # LiteRT runtime, Qualcomm accelerator plugins, and QNN SDK libraries.
        _effective_dispatch_dir = cfg.dispatch_dir or os.environ.get("LITERT_DISPATCH_DIR") or "/usr/lib"
        _log.info("Setting LITERT_DISPATCH_DIR=%r and LITERT_COMPILER_PLUGIN_DIR=%r", _effective_dispatch_dir, _effective_dispatch_dir)
        os.environ["LITERT_DISPATCH_DIR"] = _effective_dispatch_dir
        os.environ["LITERT_COMPILER_PLUGIN_DIR"] = _effective_dispatch_dir
        existing_ld = os.environ.get("LD_LIBRARY_PATH", "")
        if _effective_dispatch_dir not in existing_ld:
            os.environ["LD_LIBRARY_PATH"] = (
                f"{_effective_dispatch_dir}:{existing_ld}"
            )
            _log.debug("LD_LIBRARY_PATH prepended with %r", _effective_dispatch_dir)

        self.tokenizer = tokenizer if tokenizer is not None else build_tokenizer(cfg)
        _log.debug("Tokenizer: %s", self.tokenizer.__class__.__name__)

        # Set compile_on_init=False so we compile exactly once for whichever
        # hardware is chosen, preventing half-initialized state.
        self.interp = LiteRtInterpreter(
            model_path=cfg.model_path,
            runtime_lib=cfg.runtime_lib,
            debug=False,
            hw_mask=HW_CPU,
            compile_on_init=False,
        )

        self._cap: Dict[str, Any] = self.interp.capabilities()
        _log.debug("LiteRT capabilities: %s", self._cap)
        self._htp_enabled = False

        if cfg.prefer_htp:
            _log.info("Attempting HTP/NPU acceleration (prefer_htp=True)")
            qopts = None
            if self._cap.get("has_qualcomm_options_create", False):
                try:
                    qopts = QualcommOptions(self.interp.p)
                    qopts.create()
                    qopts.set_use_htp_preference(True)
                    if cfg.qcom_perf_mode is not None:
                        qopts.set_htp_performance_mode(int(cfg.qcom_perf_mode))
                except Exception as e:
                    _log.warning("Failed to initialize QualcommOptions: %s", e)
                    qopts = None
            else:
                _log.info("LiteRtQualcommOptionsCreate is not exported by this runtime build. Attempting plain HW_NPU compilation.")
            self._htp_enabled = bool(self.interp.enable_npu_htp(qopts, require_npu=cfg.require_htp))
            _log.info("HTP enabled: %s  NPU compiled: %s", self._htp_enabled, self.interp.npu_compiled())
        else:
            # Compile for CPU directly
            _log.debug("Compiling model for CPU (XNNPACK)")
            self.interp._recompile_or_raise()

        self.interp.allocate_tensors(signature_index=cfg.signature_index)
        self.io = self.interp.get_io_requirements()

        # ── Post-init NPU verification ────────────────────────────────────
        # Detect and surface the silent CPU fallback that occurs when the
        # NPU plugin fails auto-registration.
        self._verify_npu_status(cfg)

        _log.info(
            "NomicEmbedBackend ready: n_in=%d n_out=%d "
            "input_sizes=%s output_sizes=%s htp_active=%s npu_compiled=%s",
            self.io.num_inputs, self.io.num_outputs,
            self.io.input_req_sizes, self.io.output_req_sizes,
            self._htp_enabled, self.interp.npu_compiled(),
        )

    def _verify_npu_status(self, cfg: "BackendConfig") -> None:
        """
        Post-initialisation NPU status check.

        Raises ``RuntimeError`` if ``require_htp=True`` and NPU is not active.
        Logs a prominent WARNING if ``prefer_htp=True`` but NPU is not active,
        so that silent XNNPACK CPU fallback is never invisible in the logs.

        Root cause of silent fallback
        ------------------------------
        LiteRT's ``auto_registration.cc`` attempts to load the NPU plugin
        during ``LiteRtCreateEnvironment``.  If ``LITERT_DISPATCH_DIR`` is
        not set (or the QNN HTP libraries are absent from ``LD_LIBRARY_PATH``),
        the plugin fails with ``kLiteRtStatusErrorInvalidArgument`` and the
        runtime continues with XNNPACK CPU — no exception is raised.

        How to verify NPU is active
        ----------------------------
        After a successful initialisation with NPU, the following must all
        be true:

        * ``self._htp_enabled is True``
        * ``self.interp.npu_compiled() is True``
        * The service log contains
          ``INFO: [accelerator_registry.cc] RegisterAccelerator: name=QualcommAccelerator``
          (or similar) rather than only ``CpuAccelerator``.
        """
        npu_active = self._htp_enabled and self.interp.npu_compiled()

        if cfg.prefer_htp and not npu_active:
            msg = (
                "NPU/HTP acceleration was requested (prefer_htp=True / "
                "T2E_PREFER_HTP=1) but is NOT active. "
                "Inference will run on CPU (XNNPACK). "
                "\n\nCommon causes:"
                "\n  1. T2E_DISPATCH_DIR / LITERT_DISPATCH_DIR is not set — "
                "the QNN HTP plugin cannot be found during auto-registration."
                "\n  2. libQnnHtp.so or the LiteRT QNN accelerator plugin is "
                "absent from LD_LIBRARY_PATH."
                "\n  3. The model is not INT8-quantized — HTP requires "
                "fully-quantized graphs."
                "\n  4. The runtime was built without Qualcomm HTP support "
                "(LiteRtQualcommOptionsCreate not exported)."
                "\n\nFix checklist:"
                "\n  • Set T2E_DISPATCH_DIR to the QNN SDK lib directory "
                "(e.g. /opt/qcom/aistack/qnn/2.26.0/lib/aarch64-android)."
                "\n  • Ensure LD_LIBRARY_PATH includes that directory."
                "\n  • Quantize the model with representative-dataset INT8 "
                "calibration."
                "\n  • Set T2E_REQUIRE_HTP=1 to turn this warning into an "
                "error and prevent silent CPU fallback."
            )
            if cfg.require_htp:
                _log.error(msg)
                raise RuntimeError(msg)
            else:
                _log.warning(msg)

    def status(self) -> Dict[str, Any]:
        return {
            "capabilities": self._cap,
            "htp_enabled": self._htp_enabled,
            "npu_compiled": self.interp.npu_compiled(),
            "io": {
                "num_inputs": self.io.num_inputs,
                "num_outputs": self.io.num_outputs,
                "input_req_sizes": self.io.input_req_sizes,
                "output_req_sizes": self.io.output_req_sizes,
            },
            "tokenizer": self.tokenizer.__class__.__name__,
        }

    def _write_inputs(self, ids: np.ndarray, mask: np.ndarray):
        self.interp.write_input_bytes(0, ids.astype(np.int32, copy=False).tobytes(), lock_mode=1)
        self.interp.write_input_bytes(1, mask.astype(np.int32, copy=False).tobytes(), lock_mode=1)

    def _read_embedding(self) -> np.ndarray:
        out_bytes = self.interp.read_output_bytes(0, lock_mode=0)
        dtype = np.dtype(self.cfg.output_dtype)
        dim = (len(out_bytes) // dtype.itemsize) if self.cfg.embed_dim is None else int(self.cfg.embed_dim)
        need = dim * dtype.itemsize
        emb = np.frombuffer(out_bytes[:need], dtype=dtype, count=dim).astype(np.float32, copy=False)
        if self.cfg.normalize:
            emb = emb / (np.linalg.norm(emb) + 1e-12)
        return emb

    def embed(self, texts: List[str]) -> np.ndarray:
        _log.debug("embed: n_texts=%d", len(texts))
        ids, mask = self.tokenizer.encode_batch(texts, self.cfg.seq_len)

        # ── Diagnostic: log tensor shapes vs buffer requirements ──────────
        _log.info(
            "embed: ids.shape=%s ids.dtype=%s  mask.shape=%s mask.dtype=%s",
            ids.shape, ids.dtype, mask.shape, mask.dtype,
        )
        _log.info(
            "embed: input_req_sizes=%s  output_req_sizes=%s",
            self.io.input_req_sizes, self.io.output_req_sizes,
        )
        _log.info(
            "embed: ids[0] bytes=%d  mask[0] bytes=%d  "
            "req[0]=%d  req[1]=%s",
            ids[0:1].nbytes,
            mask[0:1].nbytes,
            self.io.input_req_sizes[0] if self.io.input_req_sizes else -1,
            self.io.input_req_sizes[1] if len(self.io.input_req_sizes) > 1 else "N/A",
        )
        # ─────────────────────────────────────────────────────────────────

        embs: List[np.ndarray] = []
        for i in range(ids.shape[0]):
            self._write_inputs(ids[i:i+1], mask[i:i+1])
            self.interp.invoke(signature_index=self.cfg.signature_index)
            embs.append(self._read_embedding())
        result = np.stack(embs, axis=0)
        _log.debug("embed: done shape=%s", result.shape)
        return result

    # ------------------------------------------------------------------
    # EmbeddingBackend interface
    # ------------------------------------------------------------------

    def embed_texts(self, texts: List[str]) -> List[List[float]]:
        """
        Embed *texts* via LiteRT and return one float32 vector per text.

        Implements :meth:`EmbeddingBackend.embed_texts`.
        """
        result: np.ndarray = self.embed(texts)   # shape [N, D]
        return result.astype(float).tolist()

    def encode_tokens(self, text: str) -> List[int]:
        """
        Tokenise *text* and return token IDs (no padding).

        Implements :meth:`EmbeddingBackend.encode_tokens`.
        """
        return self.tokenizer.encode(text)

    def close(self) -> None:
        try:
            self.interp.close()
        except Exception:
            pass


# =========================================
# Subprocess Worker Utilities (moved here)
# =========================================

_BACKEND_MODULE = "openapi_server.impl.litert_backend.backend"

_WORKER_LOCK = threading.Lock()
_WORKER_CALL_LOCK = threading.Lock()

_WORKER_PROC: Optional[subprocess.Popen] = None
_WORKER_CONN = None  # type: ignore
_WORKER_LISTENER: Optional[Listener] = None
_WORKER_KEY: Optional[str] = None


def _dbg(msg: str) -> None:
    """Debug helper for backend/worker infra — routes to the file logger."""
    try:
        _log.debug("[worker-infra] %s", msg)
    except Exception:
        pass


def ensure_list_of_strings(value: Any) -> List[str]:
    """Public helper: normalize inputs to List[str]."""
    _dbg(f"ensure_list_of_strings: raw type={type(value)}")
    if isinstance(value, str):
        return [value]
    if isinstance(value, list):
        return [(v if isinstance(v, str) else str(v)) for v in value]
    return [str(value)]


def normalize_encoding_format(encoding_format: Optional[str]) -> str:
    if not encoding_format:
        return "float"
    ef = str(encoding_format).lower().strip()
    return ef if ef in ("float", "base64") else "float"


def map_encoding_format_to_dtype(encoding_format: Optional[str]) -> str:
    if not encoding_format or encoding_format == "float":
        return "float32"
    ef = str(encoding_format).lower().strip()
    if ef in ("float32", "float16", "float64"):
        return ef
    if ef == "base64":
        return "float32"
    return "float32"


def embeddings_to_base64_rows(embs: np.ndarray) -> List[str]:
    embs32 = embs.astype(np.float32, copy=False)
    return [base64.b64encode(row.tobytes()).decode("ascii") for row in embs32]


def _cfg_key(
    model: str,
    dtype: str,
    normalize: bool,
    seq_len: int,
    prefer_htp: bool = False,
    require_htp: bool = False,
    runtime_lib: str = "libLiteRt.so",
    dispatch_dir: Optional[str] = None,
    qcom_perf_mode: Optional[int] = None,
    signature_index: int = 0,
    embed_dim: Optional[int] = None,
) -> str:
    return (
        f"model={model}|dtype={dtype}|norm={int(normalize)}|seq={seq_len}"
        f"|htp={int(prefer_htp)}|req_htp={int(require_htp)}"
        f"|lib={runtime_lib}|disp={dispatch_dir or ''}|perf={qcom_perf_mode}"
        f"|sig={signature_index}|dim={embed_dim}"
    )


def _close_worker_locked(reason: str) -> None:
    global _WORKER_PROC, _WORKER_CONN, _WORKER_LISTENER, _WORKER_KEY
    _dbg(f"close_worker_locked: reason={reason}")

    try:
        if _WORKER_CONN is not None:
            try:
                _WORKER_CONN.send({"op": "close"})
            except Exception:
                pass
            try:
                _WORKER_CONN.close()
            except Exception:
                pass
    finally:
        _WORKER_CONN = None

    try:
        if _WORKER_LISTENER is not None:
            try:
                _WORKER_LISTENER.close()
            except Exception:
                pass
    finally:
        _WORKER_LISTENER = None

    try:
        if _WORKER_PROC is not None:
            try:
                _WORKER_PROC.terminate()
            except Exception:
                pass
            try:
                _WORKER_PROC.wait(timeout=2)
            except Exception:
                pass
    finally:
        _WORKER_PROC = None
        _WORKER_KEY = None

    _dbg("close_worker_locked: done")


def _accept_with_timeout(listener: Listener, timeout_s: float):
    result = {"conn": None, "err": None}

    def _target():
        try:
            result["conn"] = listener.accept()
        except Exception as e:
            result["err"] = e

    t = threading.Thread(target=_target, daemon=True)
    t.start()
    t.join(timeout=timeout_s)

    if t.is_alive():
        raise TimeoutError(f"Timed out waiting for worker to connect after {timeout_s}s")
    if result["err"] is not None:
        raise result["err"]
    return result["conn"]


def ensure_worker(
    model: str,
    dtype: str,
    normalize: bool = True,
    seq_len: int = 128,
    prefer_htp: bool = False,
    require_htp: bool = False,
    runtime_lib: str = "libLiteRt.so",
    dispatch_dir: Optional[str] = None,
    qcom_perf_mode: Optional[int] = None,
    signature_index: int = 0,
    embed_dim: Optional[int] = None,
) -> None:
    """
    Start or reuse a persistent subprocess worker for this config.
    Uses subprocess.Popen (works even from daemonized Hypercorn workers).
    All HTP/NPU settings are forwarded to the worker via environment variables.
    """
    global _WORKER_PROC, _WORKER_CONN, _WORKER_LISTENER, _WORKER_KEY

    key = _cfg_key(
        model, dtype, normalize, seq_len,
        prefer_htp=prefer_htp, require_htp=require_htp,
        runtime_lib=runtime_lib, dispatch_dir=dispatch_dir,
        qcom_perf_mode=qcom_perf_mode, signature_index=signature_index,
        embed_dim=embed_dim,
    )

    with _WORKER_LOCK:
        if _WORKER_PROC is not None and _WORKER_KEY == key:
            if _WORKER_PROC.poll() is None and _WORKER_CONN is not None:
                _dbg(f"ensure_worker: reuse existing worker for key={key}")
                return
            _dbg("ensure_worker: worker exists but dead/disconnected; restarting")
            _close_worker_locked("dead/disconnected")

        if _WORKER_PROC is not None and _WORKER_KEY != key:
            _dbg(f"ensure_worker: config changed {_WORKER_KEY} -> {key}; restarting")
            _close_worker_locked("config changed")

        _dbg(f"ensure_worker: starting new worker key={key}")
        authkey = os.urandom(16)
        listener = Listener(("127.0.0.1", 0), authkey=authkey)
        host, port = listener.address  # type: ignore

        env = os.environ.copy()
        env["T2E_SUBPROC_WORKER"] = "1"
        env["T2E_WORKER_HOST"] = str(host)
        env["T2E_WORKER_PORT"] = str(port)
        env["T2E_WORKER_AUTH_B64"] = base64.b64encode(authkey).decode("ascii")

        # Core model settings
        env["T2E_MODEL_PATH"] = model
        env["T2E_DTYPE"] = dtype
        env["T2E_NORMALIZE"] = "1" if normalize else "0"
        env["T2E_SEQ_LEN"] = str(seq_len)
        env["T2E_RUN_ID"] = uuid.uuid4().hex[:10]

        # HTP/NPU settings — forwarded so the worker can enable NPU acceleration
        env["T2E_PREFER_HTP"] = "1" if prefer_htp else "0"
        env["T2E_REQUIRE_HTP"] = "1" if require_htp else "0"
        env["T2E_RUNTIME_LIB"] = runtime_lib
        if dispatch_dir is not None:
            env["T2E_DISPATCH_DIR"] = dispatch_dir
        if qcom_perf_mode is not None:
            env["T2E_PERF_MODE"] = str(qcom_perf_mode)
        env["T2E_SIGNATURE_INDEX"] = str(signature_index)
        if embed_dim is not None:
            env["T2E_EMBED_DIM"] = str(embed_dim)

        cmd = [sys.executable, "-u", "-m", _BACKEND_MODULE]
        _dbg(f"ensure_worker: launching {' '.join(cmd)}")
        proc = subprocess.Popen(cmd, env=env)

        conn = _accept_with_timeout(listener, timeout_s=10.0)

        _WORKER_PROC = proc
        _WORKER_LISTENER = listener
        _WORKER_CONN = conn
        _WORKER_KEY = key
        _dbg(f"ensure_worker: connected to worker pid={proc.pid}")


def call_worker_embed_sync(
    model: str,
    inputs: List[str],
    dimensions: Optional[int],
    encoding_format: str,
    timeout_s: int = 120,
    normalize: bool = True,
    seq_len: int = 128,
    prefer_htp: bool = False,
    require_htp: bool = False,
    runtime_lib: str = "libLiteRt.so",
    dispatch_dir: Optional[str] = None,
    qcom_perf_mode: Optional[int] = None,
    signature_index: int = 0,
    embed_dim: Optional[int] = None,
) -> List[Union[List[float], str]]:
    """
    Synchronous call into worker. Intended to be called via asyncio.to_thread().
    Restarts the worker once if it fails.
    All HTP/NPU settings are forwarded to the persistent worker subprocess.
    """
    ef = normalize_encoding_format(encoding_format)
    dtype = map_encoding_format_to_dtype(ef)

    _worker_kwargs = dict(
        model=model, dtype=dtype, normalize=normalize, seq_len=seq_len,
        prefer_htp=prefer_htp, require_htp=require_htp,
        runtime_lib=runtime_lib, dispatch_dir=dispatch_dir,
        qcom_perf_mode=qcom_perf_mode, signature_index=signature_index,
        embed_dim=embed_dim,
    )
    ensure_worker(**_worker_kwargs)

    with _WORKER_CALL_LOCK:
        for attempt in (1, 2):
            try:
                if _WORKER_PROC is None or _WORKER_CONN is None:
                    raise RuntimeError("Worker not available")
                if _WORKER_PROC.poll() is not None:
                    raise RuntimeError(f"Worker exited code {_WORKER_PROC.returncode}")

                _dbg(f"call_worker_embed_sync: send attempt={attempt} inputs={len(inputs)}")
                _WORKER_CONN.send({
                    "op": "embed",
                    "inputs": inputs,
                    "dimensions": dimensions,
                    "encoding_format": ef,
                })

                if not _WORKER_CONN.poll(timeout_s):
                    raise TimeoutError(f"Timed out after {timeout_s}s waiting for worker")

                resp = _WORKER_CONN.recv()
                if not resp.get("ok", False):
                    raise RuntimeError(f"Worker error: {resp.get('error')}\n{resp.get('trace','')}")

                return resp.get("data", [])

            except Exception as e:
                _dbg(f"call_worker_embed_sync: exception attempt={attempt}: {e}\n{traceback.format_exc()}")
                if attempt == 1:
                    with _WORKER_LOCK:
                        _close_worker_locked("call failed; restart")
                    ensure_worker(**_worker_kwargs)
                    continue
                raise


# ==========================
# Worker entrypoint (subproc)
# ==========================

def _worker_entry_from_env() -> None:
    if os.environ.get("T2E_SUBPROC_WORKER") != "1":
        return

    run_id = os.environ.get("T2E_RUN_ID", "noid")
    pid = os.getpid()

    # Each worker subprocess gets its own logger instance.  The logger module
    # is already initialised (it auto-initialises on first get_logger call),
    # so this just retrieves the child logger for the worker.
    _wlog = get_logger(f"worker.{run_id}")

    def wdbg(msg: str) -> None:
        _wlog.debug("[pid=%d run=%s] %s", pid, run_id, msg)

    try:
        host = os.environ["T2E_WORKER_HOST"]
        port = int(os.environ["T2E_WORKER_PORT"])
        auth = base64.b64decode(os.environ["T2E_WORKER_AUTH_B64"].encode("ascii"))

        model = os.environ["T2E_MODEL_PATH"]
        dtype = os.environ.get("T2E_DTYPE", "float32")
        normalize = os.environ.get("T2E_NORMALIZE", "1") == "1"
        seq_len = int(os.environ.get("T2E_SEQ_LEN", "128"))

        _wlog.info("Worker starting: model=%r dtype=%r normalize=%s seq_len=%d", model, dtype, normalize, seq_len)
        wdbg(f"connecting to parent at {host}:{port}")
        conn = Client((host, port), authkey=auth)
        wdbg("connected to parent")

        prefer_htp = os.environ.get("T2E_PREFER_HTP", "1") == "1"
        require_htp = os.environ.get("T2E_REQUIRE_HTP", "1") == "1"
        runtime_lib = os.environ.get("T2E_RUNTIME_LIB", "libLiteRt.so")
        dispatch_dir = os.environ.get("T2E_DISPATCH_DIR") or None
        perf_mode_str = os.environ.get("T2E_PERF_MODE")
        qcom_perf_mode = int(perf_mode_str) if perf_mode_str else None
        signature_index = int(os.environ.get("T2E_SIGNATURE_INDEX", "0"))
        embed_dim_str = os.environ.get("T2E_EMBED_DIM")
        embed_dim = int(embed_dim_str) if embed_dim_str else None

        cfg = BackendConfig(
            model_path=model,
            seq_len=seq_len,
            normalize=normalize,
            output_dtype=dtype,
            prefer_htp=prefer_htp,
            require_htp=require_htp,
            runtime_lib=runtime_lib,
            dispatch_dir=dispatch_dir,
            qcom_perf_mode=qcom_perf_mode,
            signature_index=signature_index,
            embed_dim=embed_dim,
        )

        _wlog.info(
            "Creating NomicEmbedBackend: model=%r dtype=%r norm=%s seq=%d "
            "prefer_htp=%s require_htp=%s",
            model, dtype, normalize, seq_len, prefer_htp, require_htp,
        )
        backend = NomicEmbedBackend(cfg)
        _wlog.info("NomicEmbedBackend created successfully")
        wdbg("NomicEmbedBackend created")

        wdbg("entering loop")
        while True:
            msg = conn.recv()
            op = msg.get("op")

            if op == "close":
                _wlog.info("Received close signal; shutting down worker")
                wdbg("received close")
                try:
                    conn.send({"ok": True, "closed": True})
                except Exception:
                    pass
                break

            if op != "embed":
                _wlog.warning("Unknown op=%r received; ignoring", op)
                conn.send({"ok": False, "error": f"unknown op={op}"})
                continue

            try:
                inputs = msg.get("inputs", [])
                dimensions = msg.get("dimensions")
                ef = normalize_encoding_format(msg.get("encoding_format", "float"))

                _wlog.debug("embed request: n=%d dims=%r fmt=%r", len(inputs), dimensions, ef)
                wdbg(f"embed request n={len(inputs)} dims={dimensions!r} fmt={ef!r}")
                t0 = time.time()
                embs = backend.embed(inputs)
                elapsed = time.time() - t0
                _wlog.info("embed completed: n=%d elapsed=%.3fs shape=%s", len(inputs), elapsed, getattr(embs, "shape", None))
                wdbg(f"embed done in {elapsed:.3f}s shape={getattr(embs,'shape',None)}")

                if dimensions is not None:
                    try:
                        d = int(dimensions)
                        if d > 0:
                            embs = embs[:, :d]
                    except Exception:
                        pass

                if ef == "base64":
                    out = embeddings_to_base64_rows(embs)
                else:
                    out = embs.astype(float).tolist()

                conn.send({"ok": True, "data": out})

            except Exception as e:
                _wlog.error("embed failed: %s\n%s", e, traceback.format_exc())
                conn.send({"ok": False, "error": str(e), "trace": traceback.format_exc()})

        wdbg("closing backend")
        _wlog.info("Closing backend and exiting worker")
        try:
            backend.close()
        except Exception:
            pass

        try:
            conn.close()
        except Exception:
            pass

        _wlog.info("Worker exiting cleanly (pid=%d run=%s)", pid, run_id)
        wdbg("worker exiting")
        os._exit(0)

    except Exception as e:
        # Last-resort: write to both the file logger and stderr so the error
        # is visible regardless of whether the log file is accessible.
        try:
            _wlog.critical("FATAL worker error: %s\n%s", e, traceback.format_exc())
        except Exception:
            pass
        import sys as _sys
        print(f"[T2E-WORKER pid={pid} run={run_id}] FATAL: {e}\n{traceback.format_exc()}", file=_sys.stderr, flush=True)
        os._exit(2)


_worker_entry_from_env()
