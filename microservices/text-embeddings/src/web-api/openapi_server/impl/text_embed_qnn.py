# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from __future__ import annotations

import os
import ctypes
import importlib.util
from typing import List, Optional
from ctypes import (
    c_void_p,
    c_uint32,
    c_int32,
    c_uint8,
    c_uint64,
    c_size_t,
    POINTER,
)

from openapi_server.impl.qnn_runtime.lib_provider import QnnLibrary, QnnProvider, SystemProvider
from openapi_server.impl.qnn_runtime.qnn_types import (
    Qnn_Tensor_t,
    QNN_DATATYPE_FLOAT_16,
    QNN_DATATYPE_FLOAT_32,
    QNN_DATATYPE_INT_32,
    QNN_DATATYPE_UINT_32,
    QNN_TENSORMEMTYPE_RAW,
    qnn_dtype_size_bytes,
)
from openapi_server.impl.qnn_runtime.system_structs import QnnSystemContext_BinaryInfo_t

try:
    import numpy as np
except Exception:
    np = None


# --------------------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------------------

def _prod(xs):
    p = 1
    for x in xs:
        p *= int(x)
    return int(p)


def _tensor_name(t: Qnn_Tensor_t) -> str:
    if int(t.version) == 1:
        return (t.u.v1.name or b"").decode(errors="replace")
    return (t.u.v2.name or b"").decode(errors="replace")


def _tensor_rank_dims_dtype(t: Qnn_Tensor_t):
    if int(t.version) == 1:
        rank = int(t.u.v1.rank)
        dims_ptr = t.u.v1.dimensions
        dtype = int(t.u.v1.dataType)
    else:
        rank = int(t.u.v2.rank)
        dims_ptr = t.u.v2.dimensions
        dtype = int(t.u.v2.dataType)

    dims = []
    if dims_ptr:
        for i in range(rank):
            dims.append(int(dims_ptr[i]))
    return rank, dims, dtype


def _clone_tensor_with_dims(t: Qnn_Tensor_t):
    """Clone tensor struct and deep-copy dims array. Returns (tensor_copy, dims_array_keepalive)."""
    tc = Qnn_Tensor_t()
    tc.version = t.version

    if int(t.version) == 1:
        tc.u.v1.id = t.u.v1.id
        tc.u.v1.name = t.u.v1.name
        tc.u.v1.type = t.u.v1.type
        tc.u.v1.dataFormat = t.u.v1.dataFormat
        tc.u.v1.dataType = t.u.v1.dataType
        tc.u.v1.quantizeParams = t.u.v1.quantizeParams
        tc.u.v1.rank = t.u.v1.rank
        rank = int(t.u.v1.rank)
        dims = [int(t.u.v1.dimensions[i]) for i in range(rank)] if t.u.v1.dimensions else []
        dims_arr = (c_uint32 * max(rank, 1))(*((dims + [0]) if rank == 0 else dims))
        tc.u.v1.dimensions = ctypes.cast(dims_arr, POINTER(c_uint32))
        tc.u.v1.memType = t.u.v1.memType
    else:
        tc.u.v2.id = t.u.v2.id
        tc.u.v2.name = t.u.v2.name
        tc.u.v2.type = t.u.v2.type
        tc.u.v2.dataFormat = t.u.v2.dataFormat
        tc.u.v2.dataType = t.u.v2.dataType
        tc.u.v2.quantizeParams = t.u.v2.quantizeParams
        tc.u.v2.rank = t.u.v2.rank
        rank = int(t.u.v2.rank)
        dims = [int(t.u.v2.dimensions[i]) for i in range(rank)] if t.u.v2.dimensions else []
        dims_arr = (c_uint32 * max(rank, 1))(*((dims + [0]) if rank == 0 else dims))
        tc.u.v2.dimensions = ctypes.cast(dims_arr, POINTER(c_uint32))
        tc.u.v2.memType = t.u.v2.memType
        tc.u.v2.isDynamicDimensions = t.u.v2.isDynamicDimensions
        tc.u.v2.sparseParams = t.u.v2.sparseParams
        tc.u.v2.isProduced = t.u.v2.isProduced

    return tc, dims_arr


def _set_tensor_raw_buffer(t: Qnn_Tensor_t, buf_ptr: c_void_p, buf_size: int):
    if int(t.version) == 1:
        t.u.v1.memType = QNN_TENSORMEMTYPE_RAW
        t.u.v1.clientBuf.data = buf_ptr
        t.u.v1.clientBuf.dataSize = int(buf_size)
    else:
        t.u.v2.memType = QNN_TENSORMEMTYPE_RAW
        t.u.v2.clientBuf.data = buf_ptr
        t.u.v2.clientBuf.dataSize = int(buf_size)


def _dtype_to_ctype(dtype: int):
    # Minimal mapping for typical model inputs
    if dtype == QNN_DATATYPE_INT_32:
        return c_int32
    if dtype == QNN_DATATYPE_UINT_32:
        return c_uint32
    # fallback: treat as bytes container
    return c_uint8


# --------------------------------------------------------------------------------------
# Tokenizers
# --------------------------------------------------------------------------------------

class TokenizerAdapter:
    def encode(self, text: str) -> list[int]:
        raise NotImplementedError


class SentencePieceTokenizer(TokenizerAdapter):
    def __init__(self, model_path: str):
        try:
            import sentencepiece as spm
        except Exception as e:
            raise RuntimeError(f"sentencepiece not installed: {e}")

        self.sp = spm.SentencePieceProcessor()
        if not self.sp.Load(model_path):
            raise RuntimeError(f"Failed to load SentencePiece model: {model_path}")

    def encode(self, text: str) -> list[int]:
        return list(self.sp.EncodeAsIds(text))


class HFTokenizerJSON(TokenizerAdapter):
    def __init__(self, json_path: str):
        try:
            from tokenizers import Tokenizer
        except Exception as e:
            raise RuntimeError(f"tokenizers not installed: {e}")

        self.tok = Tokenizer.from_file(json_path)

    def encode(self, text: str) -> list[int]:
        enc = self.tok.encode(text)
        return list(enc.ids)


class CustomTokenizer(TokenizerAdapter):
    def __init__(self, module_path: str, func_name: str):
        module_path = os.path.abspath(module_path)
        spec = importlib.util.spec_from_file_location("_custom_tok", module_path)
        if not spec or not spec.loader:
            raise RuntimeError(f"Cannot import custom tokenizer module: {module_path}")

        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)

        fn = getattr(mod, func_name, None)
        if not fn or not callable(fn):
            raise RuntimeError(f"Function '{func_name}' not found in {module_path}")

        self.fn = fn

    def encode(self, text: str) -> list[int]:
        out = self.fn(text)
        if not isinstance(out, (list, tuple)):
            raise RuntimeError("Custom tokenizer must return list[int]")
        return [int(x) for x in out]


def load_tokenizer_auto() -> TokenizerAdapter:
    tok_dir = os.getenv("TOKENIZER_DIR")
    if not tok_dir:
        raise RuntimeError("TOKENIZER_DIR is not set")

    if not os.path.isdir(tok_dir):
        raise RuntimeError(f"TOKENIZER_DIR does not exist: {tok_dir}")

    # 1. HF tokenizer.json
    json_path = os.path.join(tok_dir, "tokenizer.json")
    if os.path.exists(json_path):
        return HFTokenizerJSON(json_path)

    # 2. SentencePiece *.model
    for fn in os.listdir(tok_dir):
        if fn.endswith(".model"):
            return SentencePieceTokenizer(os.path.join(tok_dir, fn))

    # 3. Custom tokenizer: look for a .py file
    for fn in os.listdir(tok_dir):
        if fn.endswith(".py"):
            module_path = os.path.join(tok_dir, fn)
            # default function name "encode"
            return CustomTokenizer(module_path, "encode")

    raise RuntimeError(
        f"No tokenizer found in {tok_dir}. "
        "Expected tokenizer.json, *.model, or custom .py"
    )



# --------------------------------------------------------------------------------------
# BinaryInfo helpers
# --------------------------------------------------------------------------------------

ATTN_MASK_NAMES = {
    "attention_mask",
    "attention_masks",
    "attentionmask",
    "attentionmasks"
}

TOKEN_NAMES_FALLBACK = {
    "input_ids",
    "input_tokens",
    "input_tokens:0",
    "input_tokens_0",
    "tokens",
    "input_tokens_ids",
    "input_tokens_id"
}


class SimpleQnnEmbeddingApp:

    def __init__(
        self,
        backend_lib: str,
        system_lib: str,
        binary_path: str,
        graph_name: Optional[str],
        input_name: Optional[str],
        output_name: Optional[str],
        pad_token: int,
        assume_attention_mask: bool,
        verbose: bool,
        tokenizer: TokenizerAdapter,
    ):

        self.backend_lib = backend_lib
        self.system_lib = system_lib
        self.binary_path = binary_path
        self.graph_name = graph_name
        self.input_name = input_name
        self.output_name = output_name
        self.pad_token = pad_token
        self.assume_attention_mask = assume_attention_mask
        self.verbose = verbose
        self.tokenizer = tokenizer

        # Load QNN libraries
        self.lib = QnnLibrary(backend_name=backend_lib, system_name=system_lib)
        self.lib.load()

        pp, _n = self.lib.get_qnn_providers()
        self.provider = QnnProvider(pp[0])

        self.system_provider = None
        if self.lib.system is not None:
            spp, _sn = self.lib.get_system_providers()
            self.system_provider = SystemProvider(spp[0])

        self.backend_handle = c_void_p()
        self.device_handle = c_void_p(None)
        self.context_handle = c_void_p()
        self.graph_handle = c_void_p()
        self._sys_ctx_handle = None
        self._binary_info_ptr = None

        # Initialize and prepare once
        self._init_backend()
        self._bi, self._binary_buf, self._binary_size = self._load_binary()
        self._graph_name, self._in_meta, self._out_meta, self._resolved_input, self._resolved_output = \
            self._find_graph_and_io(self._bi)
        self._create_context(self._binary_buf, self._binary_size)
        self._retrieve_graph(self._graph_name)

    # ------------------------------------------------------------------
    @classmethod
    def from_model_path(cls, model_path: str):

        backend = os.getenv("QNN_BACKEND_LIB", "libQnnHtp.so")
        system = os.getenv("QNN_SYSTEM_LIB", "libQnnSystem.so")

        graph = os.getenv("QNN_GRAPH_NAME")
        in_name = os.getenv("QNN_INPUT_NAME")
        out_name = os.getenv("QNN_OUTPUT_NAME")

        pad_token = int(os.getenv("QNN_PAD_TOKEN", "0"))
        verbose   = os.getenv("QNN_VERBOSE", "0") == "1"

        tokenizer = load_tokenizer_auto()

        return cls(
            backend_lib=backend,
            system_lib=system,
            binary_path=model_path,
            graph_name=graph,
            input_name=in_name,
            output_name=out_name,
            pad_token=pad_token,
            assume_attention_mask=True,
            verbose=verbose,
            tokenizer=tokenizer
        )

    # ------------------------------------------------------------------
    def close(self):
        try:
            if self.system_provider and self._sys_ctx_handle:
                self.system_provider.systemContextFree(self._sys_ctx_handle)
        finally:
            self._sys_ctx_handle = None
            self._binary_info_ptr = None

    def _init_backend(self):
        null_cfg = POINTER(c_void_p)()
        rc = self.provider.backendCreate(
            c_void_p(None),
            ctypes.byref(null_cfg),
            ctypes.byref(self.backend_handle)
        )
        if rc != 0:
            raise RuntimeError(f"backendCreate failed rc={rc}")

        # optional device
        try:
            null_dev_cfg = POINTER(c_void_p)()
            rc2 = self.provider.deviceCreate(
                c_void_p(None),
                ctypes.byref(null_dev_cfg),
                ctypes.byref(self.device_handle)
            )
            if rc2 != 0:
                self.device_handle = c_void_p(None)
        except Exception:
            self.device_handle = c_void_p(None)

    def _load_binary(self):
        if self.system_provider is None:
            raise RuntimeError("System provider not available.")

        data = open(self.binary_path, "rb").read()
        buf = (c_uint8 * len(data)).from_buffer_copy(data)
        buf_ptr = ctypes.cast(buf, c_void_p)

        sys_ctx = c_void_p()
        rc = self.system_provider.systemContextCreate(ctypes.byref(sys_ctx))
        if rc != 0:
            raise RuntimeError(f"systemContextCreate failed rc={rc}")

        out_bi = POINTER(QnnSystemContext_BinaryInfo_t)()
        out_size = c_size_t(0)

        rc = self.system_provider.systemContextGetBinaryInfo(
            sys_ctx,
            buf_ptr,
            c_uint64(len(data)),
            ctypes.byref(out_bi),
            ctypes.byref(out_size)
        )

        if rc != 0 or not out_bi:
            self.system_provider.systemContextFree(sys_ctx)
            raise RuntimeError(f"systemContextGetBinaryInfo failed rc={rc}")

        self._sys_ctx_handle = sys_ctx
        self._binary_info_ptr = out_bi

        return out_bi.contents, buf, len(data)

    def _find_graph_and_io(self, bi):

        ver = int(bi.version)
        if ver == 1:
            info = bi.u.contextBinaryInfoV1
        elif ver == 2:
            info = bi.u.contextBinaryInfoV2
        else:
            info = bi.u.contextBinaryInfoV3

        n = int(info.numGraphs)
        graphs = info.graphs

        if n == 0:
            raise RuntimeError("No graphs found in QNN binary.")

        idx = 0
        if self.graph_name:
            wanted = self.graph_name.encode()
            for i in range(n):
                gi = graphs[i]
                gver = int(gi.version)
                if gver == 1:
                    name = gi.u.graphInfoV1.graphName
                elif gver == 2:
                    name = gi.u.graphInfoV2.graphName
                else:
                    name = gi.u.graphInfoV3.graphName
                if name == wanted:
                    idx = i
                    break

        gi = graphs[idx]
        gver = int(gi.version)
        if gver == 1:
            g = gi.u.graphInfoV1
        elif gver == 2:
            g = gi.u.graphInfoV2
        else:
            g = gi.u.graphInfoV3

        gname = (g.graphName or b"").decode(errors="replace")

        n_in, in_ptr = int(g.numGraphInputs), g.graphInputs
        n_out, out_ptr = int(g.numGraphOutputs), g.graphOutputs

        in_names = [_tensor_name(in_ptr[i]) for i in range(n_in)]
        out_names = [_tensor_name(out_ptr[i]) for i in range(n_out)]

        input_name = self.input_name or next(
            (cand for cand in TOKEN_NAMES_FALLBACK if cand in in_names),
            in_names[0]
        )

        output_name = self.output_name or (
            "embeddings" if "embeddings" in out_names else out_names[0]
        )

        return gname, (n_in, in_ptr, in_names), (n_out, out_ptr, out_names), input_name, output_name

    def _create_context(self, binary_buf, binary_size):
        null_ctx_cfg = POINTER(c_void_p)()
        rc = self.provider.contextCreateFromBinary(
            self.backend_handle,
            self.device_handle,
            ctypes.byref(null_ctx_cfg),
            ctypes.cast(binary_buf, c_void_p),
            c_size_t(binary_size),
            ctypes.byref(self.context_handle),
            c_void_p(None)
        )
        if rc != 0:
            raise RuntimeError(f"contextCreateFromBinary failed rc={rc}")

    def _retrieve_graph(self, graph_name: str):
        rc = self.provider.graphRetrieve(
            self.context_handle,
            graph_name.encode(),
            ctypes.byref(self.graph_handle)
        )
        if rc != 0:
            raise RuntimeError(f"graphRetrieve('{graph_name}') failed rc={rc}")

    def embed_texts(self, texts: List[str]) -> List[List[float]]:

        if self.tokenizer is None:
            raise RuntimeError("Tokenizer not available.")

        vectors = []

        for text in texts:
            tokens = self.tokenizer.encode(text)
            vec = self._run_single(tokens)
            vectors.append(vec)

        return vectors

    def _run_single(self, tokens: List[int]) -> List[float]:

        n_in, in_ptr, in_names = self._in_meta
        n_out, out_ptr, out_names = self._out_meta
        input_name = self._resolved_input
        output_name = self._resolved_output

        tok_len = len(tokens)

        # Build inputs
        inputs = (Qnn_Tensor_t * n_in)()
        input_dims_keepalive = []
        input_bufs_keepalive = []

        found_input = False

        for i in range(n_in):
            t_src = in_ptr[i]
            t, dims_arr = _clone_tensor_with_dims(t_src)
            input_dims_keepalive.append(dims_arr)

            name = _tensor_name(t)
            _, dims, dtype = _tensor_rank_dims_dtype(t)

            elem_sz = qnn_dtype_size_bytes(dtype)
            n_elem = _prod(dims) if dims else 1
            buf_sz = n_elem * elem_sz

            if name == input_name:
                found_input = True

                arr_t = _dtype_to_ctype(dtype)

                vec = [self.pad_token] * n_elem
                for j, tok in enumerate(tokens[:n_elem]):
                    vec[j] = int(tok)

                raw = (arr_t * n_elem)(*vec)
                input_bufs_keepalive.append(raw)

                _set_tensor_raw_buffer(t, ctypes.cast(raw, c_void_p), buf_sz)

            elif name.lower() in ATTN_MASK_NAMES and self.assume_attention_mask:

                arr_t = _dtype_to_ctype(dtype)
                mask = [1 if j < tok_len else 0 for j in range(n_elem)]
                raw = (arr_t * n_elem)(*mask)

                input_bufs_keepalive.append(raw)
                _set_tensor_raw_buffer(t, ctypes.cast(raw, c_void_p), buf_sz)

            else:
                raw = (c_uint8 * buf_sz)()
                input_bufs_keepalive.append(raw)
                _set_tensor_raw_buffer(t, ctypes.cast(raw, c_void_p), buf_sz)

            inputs[i] = t

        if not found_input:
            raise RuntimeError(
                f"Input tensor '{input_name}' not found. Available: {in_names}"
            )

        # Build outputs
        outputs = (Qnn_Tensor_t * n_out)()
        output_dims_keepalive = []
        output_bufs_keepalive = []

        selected_idx = None
        for i in range(n_out):
            if _tensor_name(out_ptr[i]) == output_name:
                selected_idx = i
                break

        if selected_idx is None:
            raise RuntimeError(
                f"Output tensor '{output_name}' not found among {out_names}"
            )

        for i in range(n_out):
            t_src = out_ptr[i]
            t, dims_arr = _clone_tensor_with_dims(t_src)

            output_dims_keepalive.append(dims_arr)

            _, dims, dtype = _tensor_rank_dims_dtype(t)
            elem_sz = qnn_dtype_size_bytes(dtype)
            n_elem = _prod(dims) if dims else 1
            buf_sz = n_elem * elem_sz

            raw = (c_uint8 * buf_sz)()
            output_bufs_keepalive.append(raw)

            _set_tensor_raw_buffer(t, ctypes.cast(raw, c_void_p), buf_sz)

            outputs[i] = t

        # Execute
        rc = self.provider.graphExecute(
            self.graph_handle,
            ctypes.cast(inputs, POINTER(Qnn_Tensor_t)),
            c_uint32(n_in),
            ctypes.cast(outputs, POINTER(Qnn_Tensor_t)),
            c_uint32(n_out),
            c_void_p(None),
            c_void_p(None)
        )

        if rc != 0:
            raise RuntimeError(f"graphExecute failed rc={rc}")

        # Decode output
        out_t = outputs[selected_idx]
        _, odims, odtype = _tensor_rank_dims_dtype(out_t)

        raw_bytes = bytes(output_bufs_keepalive[selected_idx])

        if np is not None:
            if odtype == QNN_DATATYPE_FLOAT_32:
                arr = np.frombuffer(raw_bytes, dtype=np.float32)
            elif odtype == QNN_DATATYPE_FLOAT_16:
                arr = np.frombuffer(raw_bytes, dtype=np.float16).astype(np.float32)
            else:
                raise RuntimeError(f"Unsupported dtype {int(odtype)}")

            return arr.astype("float32").tolist()

        else:
            if odtype != QNN_DATATYPE_FLOAT_32:
                raise RuntimeError("NumPy unavailable but output is not float32")

            import struct
            count = len(raw_bytes) // 4
            return list(struct.unpack("<" + "f" * count, raw_bytes))
