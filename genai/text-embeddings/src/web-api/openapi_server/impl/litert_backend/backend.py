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
    prefer_htp: bool = False
    require_htp: bool = False
    qcom_perf_mode: Optional[int] = None
    signature_index: int = 0
    output_dtype: str = "float32"
    embed_dim: Optional[int] = None

    tokenizer_dir: Optional[str] = os.environ.get("TOKENIZER_DIR")

    sentencepiece_model: Optional[str] = None


class Tokenizer:
    def encode_batch(self, texts: List[str], seq_len: int) -> Tuple[np.ndarray, np.ndarray]:
        raise NotImplementedError


class HFTokenizer(Tokenizer):
    """
    Robust local HuggingFace tokenizer loader.
    Works if tokenizer_dir contains tokenizer.json.
    """
    def __init__(self, tokenizer_dir: str):
        self.tokenizer_dir = tokenizer_dir
        self.tok = None

        # Try standard AutoTokenizer first (if config exists)
        try:
            from transformers import AutoTokenizer
            self.tok = AutoTokenizer.from_pretrained(tokenizer_dir, local_files_only=True, use_fast=True)
            return
        except Exception:
            pass

        # Fallback: tokenizer.json directly
        tok_json = os.path.join(tokenizer_dir, "tokenizer.json")
        if os.path.exists(tok_json):
            from transformers import PreTrainedTokenizerFast
            self.tok = PreTrainedTokenizerFast(tokenizer_file=tok_json)
            return

        raise ValueError(
            f"Tokenizer dir '{tokenizer_dir}' is not a valid HF tokenizer folder. "
            f"Expected tokenizer.json or a standard HF tokenizer layout."
        )

    def encode_batch(self, texts: List[str], seq_len: int) -> Tuple[np.ndarray, np.ndarray]:
        enc = self.tok(
            texts,
            padding="max_length",
            truncation=True,
            max_length=int(seq_len),
            return_tensors="np",
        )
        input_ids = enc["input_ids"].astype(np.int32, copy=False)
        attn = enc.get("attention_mask")
        if attn is None:
            attn = (input_ids != 0).astype(np.int32, copy=False)
        else:
            attn = attn.astype(np.int32, copy=False)

        # Sanity check: huge ids usually mean mismatched tokenizer/vocab
        if input_ids.size and int(input_ids.max()) > 10_000_000:
            raise RuntimeError(
                f"Suspicious token id max={int(input_ids.max())}. Tokenizer likely doesn't match model vocab."
            )
        return input_ids, attn


class SafePadTokenizer(Tokenizer):
    """Fallback to keep execution possible; NOT meaningful."""
    def encode_batch(self, texts: List[str], seq_len: int) -> Tuple[np.ndarray, np.ndarray]:
        B = len(texts)
        return np.zeros((B, seq_len), np.int32), np.zeros((B, seq_len), np.int32)


def build_tokenizer(cfg: BackendConfig) -> Tokenizer:
    if cfg.tokenizer_dir:
        return HFTokenizer(cfg.tokenizer_dir)
    return SafePadTokenizer()


class NomicEmbedBackend:
    def __init__(self, cfg: BackendConfig, tokenizer: Optional[Tokenizer] = None):
        self.cfg = cfg

        if cfg.dispatch_dir:
            os.environ["LITERT_DISPATCH_DIR"] = cfg.dispatch_dir
            os.environ["LD_LIBRARY_PATH"] = f"{cfg.dispatch_dir}:{os.environ.get('LD_LIBRARY_PATH','')}"

        self.tokenizer = tokenizer if tokenizer is not None else build_tokenizer(cfg)

        self.interp = LiteRtInterpreter(
            model_path=cfg.model_path,
            runtime_lib=cfg.runtime_lib,
            debug=False,
            hw_mask=HW_CPU,
            compile_on_init=True,
        )

        self._cap: Dict[str, Any] = self.interp.capabilities()
        self._htp_enabled: bool = False

        if cfg.prefer_htp and self._cap.get("has_qualcomm_options_create", False):
            qopts = QualcommOptions(self.interp.p)
            qopts.create()
            qopts.set_use_htp_preference(True)
            if cfg.qcom_perf_mode is not None:
                qopts.set_htp_performance_mode(int(cfg.qcom_perf_mode))
            self._htp_enabled = bool(self.interp.enable_npu_htp(qopts, require_npu=cfg.require_htp))

        self.interp.allocate_tensors(signature_index=cfg.signature_index)
        self.io = self.interp.get_io_requirements()

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
        ids, mask = self.tokenizer.encode_batch(texts, self.cfg.seq_len)
        embs: List[np.ndarray] = []
        for i in range(ids.shape[0]):
            self._write_inputs(ids[i:i+1], mask[i:i+1])
            self.interp.invoke(signature_index=self.cfg.signature_index)
            embs.append(self._read_embedding())
        return np.stack(embs, axis=0)

    def close(self):
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
    """Debug helper for backend/worker infra: prints flush immediately."""
    try:
        pid = os.getpid()
        tid = threading.get_ident()
        print(f"[T2E-BE-DBG pid={pid} tid={tid}] {msg}", flush=True)
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


def _cfg_key(model: str, dtype: str, normalize: bool, seq_len: int) -> str:
    return f"model={model}|dtype={dtype}|norm={int(normalize)}|seq={seq_len}"


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


def ensure_worker(model: str, dtype: str, normalize: bool = True, seq_len: int = 128) -> None:
    """
    Start or reuse a persistent subprocess worker for this config.
    Uses subprocess.Popen (works even from daemonized Hypercorn workers).
    """
    global _WORKER_PROC, _WORKER_CONN, _WORKER_LISTENER, _WORKER_KEY

    key = _cfg_key(model, dtype, normalize, seq_len)

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

        env["T2E_MODEL_PATH"] = model
        env["T2E_DTYPE"] = dtype
        env["T2E_NORMALIZE"] = "1" if normalize else "0"
        env["T2E_SEQ_LEN"] = str(seq_len)
        env["T2E_RUN_ID"] = uuid.uuid4().hex[:10]

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
) -> List[Union[List[float], str]]:
    """
    Synchronous call into worker. Intended to be called via asyncio.to_thread().
    Restarts the worker once if it fails.
    """
    ef = normalize_encoding_format(encoding_format)
    dtype = map_encoding_format_to_dtype(ef)

    ensure_worker(model=model, dtype=dtype, normalize=True, seq_len=128)

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
                    ensure_worker(model=model, dtype=dtype, normalize=True, seq_len=128)
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

    def wdbg(msg: str) -> None:
        print(f"[T2E-WORKER pid={pid} run={run_id}] {msg}", flush=True)

    try:
        host = os.environ["T2E_WORKER_HOST"]
        port = int(os.environ["T2E_WORKER_PORT"])
        auth = base64.b64decode(os.environ["T2E_WORKER_AUTH_B64"].encode("ascii"))

        model = os.environ["T2E_MODEL_PATH"]
        dtype = os.environ.get("T2E_DTYPE", "float32")
        normalize = os.environ.get("T2E_NORMALIZE", "1") == "1"
        seq_len = int(os.environ.get("T2E_SEQ_LEN", "128"))

        wdbg(f"connecting to parent at {host}:{port}")
        conn = Client((host, port), authkey=auth)
        wdbg("connected to parent")

        cfg = BackendConfig(
            model_path=model,
            seq_len=seq_len,
            normalize=normalize,
            output_dtype=dtype,
        )

        wdbg(f"creating NomicEmbedBackend... model={model!r} dtype={dtype!r} norm={normalize} seq={seq_len}")
        backend = NomicEmbedBackend(cfg)
        wdbg("NomicEmbedBackend created")

        wdbg("entering loop")
        while True:
            msg = conn.recv()
            op = msg.get("op")

            if op == "close":
                wdbg("received close")
                try:
                    conn.send({"ok": True, "closed": True})
                except Exception:
                    pass
                break

            if op != "embed":
                conn.send({"ok": False, "error": f"unknown op={op}"})
                continue

            try:
                inputs = msg.get("inputs", [])
                dimensions = msg.get("dimensions")
                ef = normalize_encoding_format(msg.get("encoding_format", "float"))

                wdbg(f"embed request n={len(inputs)} dims={dimensions!r} fmt={ef!r}")
                t0 = time.time()
                embs = backend.embed(inputs)
                wdbg(f"embed done in {(time.time()-t0):.3f}s shape={getattr(embs,'shape',None)}")

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
                conn.send({"ok": False, "error": str(e), "trace": traceback.format_exc()})

        wdbg("closing backend")
        try:
            backend.close()
        except Exception:
            pass

        try:
            conn.close()
        except Exception:
            pass

        wdbg("worker exiting")
        os._exit(0)

    except Exception as e:
        print(f"[T2E-WORKER pid={pid} run={run_id}] FATAL: {e}\n{traceback.format_exc()}", flush=True)
        os._exit(2)


_worker_entry_from_env()
