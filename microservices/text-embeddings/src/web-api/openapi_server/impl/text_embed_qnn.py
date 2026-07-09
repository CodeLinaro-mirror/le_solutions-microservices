# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from __future__ import annotations

import os
import ctypes
import importlib.util
import time
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

from openapi_server.logger.logger_config import LoggerConfig
from openapi_server.impl.qnn_runtime.lib_provider import QnnLibrary, QnnProvider, SystemProvider, CORE_LOG_CB

LoggerConfig.initialize()
logger = LoggerConfig.get_logger(__name__)
from openapi_server.impl.embedding_backend import EmbeddingBackend
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
from openapi_server.impl.qnn_runtime.utils_dump import dump_tensors

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


def load_tokenizer_auto(model_path: Optional[str] = None) -> TokenizerAdapter:
    # 1. Try to find dynamically near the model_path if provided
    if model_path:
        model_dir = os.path.dirname(model_path)
        if os.path.isdir(model_dir):
            json_path = os.path.join(model_dir, "tokenizer.json")
            if os.path.exists(json_path):
                logger.info(f"Dynamically discovered tokenizer.json near model: {json_path}")
                return HFTokenizerJSON(json_path)
            # Recursively walk to find it
            try:
                for root, dirs, files in os.walk(model_dir):
                    depth = root[len(model_dir):].count(os.sep)
                    if depth > 2:
                        dirs.clear()
                        continue
                    if "tokenizer.json" in files:
                        found_json = os.path.join(root, "tokenizer.json")
                        logger.info(f"Dynamically discovered tokenizer.json in model subdirectory: {found_json}")
                        return HFTokenizerJSON(found_json)
            except Exception as e:
                logger.warning(f"Error while dynamically searching for tokenizer.json near {model_path}: {e}")

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
                    logger.info(f"Dynamically discovered tokenizer.json in T2E_MODEL_DIR: {found_json}")
                    return HFTokenizerJSON(found_json)
        except Exception as e:
            logger.warning(f"Error while dynamically searching for tokenizer.json in T2E_MODEL_DIR {models_dir}: {e}")

    # 3. Fallback to TOKENIZER_DIR
    tok_dir = os.getenv("TOKENIZER_DIR")
    if not tok_dir:
        raise RuntimeError("TOKENIZER_DIR is not set")

    if not os.path.isdir(tok_dir):
        raise RuntimeError(f"TOKENIZER_DIR does not exist: {tok_dir}")

    # 1. HF tokenizer.json (primary — used by Nomic Embed Text and most HF models)
    json_path = os.path.join(tok_dir, "tokenizer.json")
    if os.path.exists(json_path):
        return HFTokenizerJSON(json_path)

    # 2. Custom tokenizer: look for a .py file with an encode() function
    for fn in os.listdir(tok_dir):
        if fn.endswith(".py"):
            module_path = os.path.join(tok_dir, fn)
            return CustomTokenizer(module_path, "encode")

    raise RuntimeError(
        f"No tokenizer found in {tok_dir}. "
        "Expected tokenizer.json (HuggingFace format) or a custom .py with an encode() function."
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


class SimpleQnnEmbeddingApp(EmbeddingBackend):
    """
    QNN binary (.bin) embedding backend.

    Implements :class:`EmbeddingBackend` so it can be used interchangeably
    with :class:`~openapi_server.impl.litert_backend.backend.NomicEmbedBackend`
    inside ``EmbeddingsApiImpl._get_embeddings_from_component``.

    Typical usage
    -------------
    ::

        backend = SimpleQnnEmbeddingApp.from_model_path("/opt/embed_gen/model.bin")
        vectors = backend.embed_texts(["hello world"])
        backend.close()
    """

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
        self.verbose = True
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
        self.logger_handle = c_void_p()
        self.device_handle = c_void_p(None)
        self.context_handle = c_void_p()
        self.graph_handle = c_void_p()
        self._sys_ctx_handle = None
        self._binary_info_ptr = None
        self.dlc_handle = c_void_p(None)

        # Initialize and prepare once
        self._init_logging()
        self._init_backend()
        if self.binary_path.lower().endswith(".dlc"):
            self._load_and_compose_dlc()
        else:
            self._bi, self._binary_buf, self._binary_size = self._load_binary()
            self._graph_name, self._in_meta, self._out_meta, self._resolved_input, self._resolved_output = \
                self._find_graph_and_io(self._bi)
            self._create_context(self._binary_buf, self._binary_size)
            self._retrieve_graph(self._graph_name)

    # ------------------------------------------------------------------
    def _load_and_compose_dlc(self):
        logger.info(f"SimpleQnnEmbeddingApp._load_and_compose_dlc: entry with DLC path {self.binary_path}")
        if self.system_provider is None:
            raise RuntimeError("System provider not available for QNN DLC loading.")

        # 1. Create context
        null_ctx_cfg = POINTER(POINTER(c_void_p))()
        logger.info("SimpleQnnEmbeddingApp._load_and_compose_dlc: creating QNN context...")
        rc = self.provider.contextCreate(
            self.backend_handle,
            self.device_handle,
            null_ctx_cfg,
            ctypes.byref(self.context_handle)
        )
        if rc != 0:
            raise RuntimeError(f"contextCreate failed rc={rc}")
        logger.info(f"SimpleQnnEmbeddingApp._load_and_compose_dlc: context created successfully. Handle: {self.context_handle}")

        # 2. Create DLC handle from file
        dlc = c_void_p()
        logger.info(f"SimpleQnnEmbeddingApp._load_and_compose_dlc: loading DLC file {self.binary_path}...")
        rc = self.system_provider.systemDlcCreateFromFile(
            self.logger_handle,
            self.binary_path.encode("utf-8"),
            ctypes.byref(dlc)
        )
        if rc != 0:
            raise RuntimeError(f"systemDlcCreateFromFile failed rc={rc}")
        self.dlc_handle = dlc
        logger.info(f"SimpleQnnEmbeddingApp._load_and_compose_dlc: systemDlcCreateFromFile succeeded. Handle: {self.dlc_handle}")

        # 3. Compose graphs
        from openapi_server.impl.qnn_runtime.system_structs import QnnSystemContext_GraphInfo_t
        graphs_pp = POINTER(POINTER(c_void_p))()
        num_graphs = c_uint32(0)

        interface_addr = self.provider._ptr_val
        logger.info(f"SimpleQnnEmbeddingApp._load_and_compose_dlc: composing graphs using interface_addr={hex(interface_addr)}...")

        rc = self.system_provider.systemDlcComposeGraphs(
            self.dlc_handle,
            POINTER(POINTER(c_void_p))(),  # graphConfigs
            0,                             # numGraphConfigs
            self.backend_handle,
            self.context_handle,
            interface_addr,
            1,                             # graphVersion (1 = QNN_SYSTEM_CONTEXT_GRAPH_INFO_VERSION_1)
            ctypes.byref(graphs_pp),
            ctypes.byref(num_graphs)
        )

        n_graphs = int(num_graphs.value)
        graphs_val = ctypes.cast(graphs_pp, c_void_p).value
        logger.info(f"SimpleQnnEmbeddingApp._load_and_compose_dlc: systemDlcComposeGraphs returned rc={rc}, num_graphs={n_graphs}")

        if rc != 0 or n_graphs == 0 or not graphs_val:
            raise RuntimeError(f"systemDlcComposeGraphs failed or no graphs composed. rc={rc}")

        # Cast raw pointer output to QnnSystemContext_GraphInfo_t pointer array
        graphs_pp_typed = ctypes.cast(graphs_pp, POINTER(QnnSystemContext_GraphInfo_t))

        idx = 0
        if self.graph_name:
            wanted = self.graph_name.encode()
            logger.info(f"SimpleQnnEmbeddingApp._load_and_compose_dlc: searching for graph named {self.graph_name}...")
            for i in range(n_graphs):
                g_info = graphs_pp_typed[i]
                gver = int(g_info.version)
                if gver == 1:
                    name = g_info.u.graphInfoV1.graphName
                elif gver == 2:
                    name = g_info.u.graphInfoV2.graphName
                else:
                    name = g_info.u.graphInfoV3.graphName
                logger.info(f"SimpleQnnEmbeddingApp._load_and_compose_dlc: found graph index {i} name: {name.decode(errors='replace')}")
                if name == wanted:
                    idx = i
                    break

        g_info = graphs_pp_typed[idx]
        gver = int(g_info.version)
        if gver == 1:
            g = g_info.u.graphInfoV1
        elif gver == 2:
            g = g_info.u.graphInfoV2
        else:
            g = g_info.u.graphInfoV3

        self._graph_name = (g.graphName or b"").decode(errors="replace")
        logger.info(f"SimpleQnnEmbeddingApp._load_and_compose_dlc: selected graph index {idx} with name {self._graph_name}")

        n_in, in_ptr = int(g.numGraphInputs), g.graphInputs
        n_out, out_ptr = int(g.numGraphOutputs), g.graphOutputs

        in_names = [_tensor_name(in_ptr[i]) for i in range(n_in)]
        out_names = [_tensor_name(out_ptr[i]) for i in range(n_out)]
        logger.info(f"SimpleQnnEmbeddingApp._load_and_compose_dlc: graph inputs: {in_names}, graph outputs: {out_names}")

        self._in_meta = (n_in, in_ptr, in_names)
        self._out_meta = (n_out, out_ptr, out_names)

        self._resolved_input = self.input_name or next(
            (cand for cand in TOKEN_NAMES_FALLBACK if cand in in_names),
            in_names[0] if in_names else None
        )

        self._resolved_output = self.output_name or (
            "embeddings" if "embeddings" in out_names else (out_names[0] if out_names else None)
        )
        logger.info(f"SimpleQnnEmbeddingApp._load_and_compose_dlc: resolved_input={self._resolved_input}, resolved_output={self._resolved_output}")

        # 4. Retrieve graph handle
        logger.info(f"SimpleQnnEmbeddingApp._load_and_compose_dlc: retrieving graph handle for {self._graph_name}...")
        self._retrieve_graph(self._graph_name)
        logger.info(f"SimpleQnnEmbeddingApp._load_and_compose_dlc: retrieved graph handle successfully. Handle: {self.graph_handle}")

        # 5. Finalize composed graph
        logger.info("SimpleQnnEmbeddingApp._load_and_compose_dlc: finalising composed QNN graph for DLC...")
        rc = self.provider.graphFinalize(self.graph_handle, c_void_p(None), c_void_p(None))
        if rc != 0:
            raise RuntimeError(f"graphFinalize failed rc={rc}")
        logger.info("SimpleQnnEmbeddingApp._load_and_compose_dlc: composed QNN graph finalized successfully!")

    # ------------------------------------------------------------------
    def _init_logging(self):
        """Create a QNN logger and attach CORE_LOG_CB so backend messages are visible."""
        # Use VERBOSE (4) when verbose mode is requested, INFO (2) otherwise.
        log_level = 4 if self.verbose else 2
        rc = self.provider.logCreate(
            CORE_LOG_CB,
            ctypes.c_int(log_level),
            ctypes.byref(self.logger_handle)
        )
        if rc != 0:
            # Non-fatal: continue without a logger rather than aborting startup.
            self.logger_handle = c_void_p()

    # ------------------------------------------------------------------
    @classmethod
    def from_model_path(cls, model_path: str):
        backend = os.getenv("QNN_BACKEND_LIB", "libQnnHtp.so")
        system = os.getenv("QNN_SYSTEM_LIB", "libQnnSystem.so")

        graph = os.getenv("QNN_GRAPH_NAME")
        in_name = os.getenv("QNN_INPUT_NAME")
        out_name = os.getenv("QNN_OUTPUT_NAME")

        pad_token = int(os.getenv("QNN_PAD_TOKEN", "0"))

        tokenizer = load_tokenizer_auto(model_path)

        return cls(
            backend_lib=backend,
            system_lib=system,
            binary_path=model_path,
            graph_name=graph,
            input_name=in_name,
            output_name=out_name,
            pad_token=pad_token,
            assume_attention_mask=True,
            tokenizer=tokenizer
        )

    # ------------------------------------------------------------------
    def __del__(self):
        try:
            self.close()
        except Exception as e:
            logger.error(f"An error occurred: {e}")

    def close(self):
        # 0. Free DLC context
        try:
            if self.system_provider and self.dlc_handle and self.dlc_handle.value:
                self.system_provider.systemDlcFree(self.dlc_handle)
        except Exception as e:
            logger.error(f"An error occurred: {e}")
        self.dlc_handle = c_void_p(None)

        # 1. Free system context (holds binary-info pointer; must go first)
        try:
            if self.system_provider and self._sys_ctx_handle:
                self.system_provider.systemContextFree(self._sys_ctx_handle)
        except Exception as e:
            logger.error(f"An error occurred: {e}")
        self._sys_ctx_handle = None
        self._binary_info_ptr = None

        # 2. Free QNN context (owns graph handles; must be freed before backend)
        try:
            if self.context_handle and self.context_handle.value:
                self.provider.contextFree(self.context_handle, c_void_p(None))
        except Exception as e:
            logger.error(f"An error occurred: {e}")
        self.context_handle = c_void_p()

        # 3. Free device
        try:
            if self.device_handle and self.device_handle.value:
                self.provider.deviceFree(self.device_handle)
        except Exception as e:
            logger.error(f"An error occurred: {e}")
        self.device_handle = c_void_p(None)

        # 4. Free backend
        try:
            if self.backend_handle and self.backend_handle.value:
                self.provider.backendFree(self.backend_handle)
        except Exception as e:
            logger.error(f"An error occurred: {e}")
        self.backend_handle = c_void_p()

        # 5. Free logger (must be last – backend may still log during its own teardown)
        try:
            if self.logger_handle and self.logger_handle.value:
                self.provider.logFree(self.logger_handle)
        except Exception as e:
            logger.error(f"An error occurred: {e}")

        self.logger_handle = c_void_p()

    def _init_backend(self):
        # Pass a null POINTER(POINTER(c_void_p)) — equivalent to NULL const T** in C.
        # Using byref(POINTER(c_void_p)()) would pass a non-null pointer-to-null-pointer,
        # which is technically incorrect per the QNN API contract.
        null_cfg = POINTER(POINTER(c_void_p))()
        rc = self.provider.backendCreate(
            self.logger_handle,
            null_cfg,
            ctypes.byref(self.backend_handle)
        )
        if rc != 0:
            raise RuntimeError(f"backendCreate failed rc={rc}")

        # optional device
        try:
            null_dev_cfg = POINTER(POINTER(c_void_p))()
            rc2 = self.provider.deviceCreate(
                self.logger_handle,
                null_dev_cfg,
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
        # Pass a null POINTER(POINTER(c_void_p)) for configs — equivalent to NULL const T** in C.
        null_ctx_cfg = POINTER(POINTER(c_void_p))()
        rc = self.provider.contextCreateFromBinary(
            self.backend_handle,
            self.device_handle,
            null_ctx_cfg,
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

    # ------------------------------------------------------------------
    # EmbeddingBackend interface
    # ------------------------------------------------------------------

    def embed_texts(self, texts: List[str]) -> List[List[float]]:
        """
        Embed *texts* via QNN HTP and return one float32 vector per text.

        Implements :meth:`EmbeddingBackend.embed_texts`.
        """
        logger.info(f"SimpleQnnEmbeddingApp.embed_texts: embedding batch of {len(texts)} texts")
        if self.tokenizer is None:
            logger.error("SimpleQnnEmbeddingApp.embed_texts: tokenizer is not available")
            raise RuntimeError("Tokenizer not available.")

        vectors = []
        for idx, text in enumerate(texts):
            logger.debug(f"SimpleQnnEmbeddingApp.embed_texts: processing sequence {idx + 1}/{len(texts)}")
            t0 = time.time()
            tokens = self.tokenizer.encode(text)
            logger.debug(f"SimpleQnnEmbeddingApp.embed_texts: tokenized string length={len(text)} into {len(tokens)} tokens: {tokens[:15]}")
            vec = self._run_single(tokens)
            logger.debug(f"SimpleQnnEmbeddingApp.embed_texts: sequence {idx + 1} execution completed in {time.time() - t0:.4f}s")
            vectors.append(vec)

        logger.info(f"SimpleQnnEmbeddingApp.embed_texts: successfully completed embedding batch")
        return vectors

    def encode_tokens(self, text: str) -> List[int]:
        """
        Tokenise *text* and return token IDs (no padding).

        Implements :meth:`EmbeddingBackend.encode_tokens`.
        """
        if self.tokenizer is None:
            raise RuntimeError("Tokenizer not available.")
        return self.tokenizer.encode(text)

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

        dump_tensors(inputs, n_in, kind="INPUTS")
        dump_tensors(outputs, n_out, kind="OUTPUTS")

        # Execute
        logger.debug(f"SimpleQnnEmbeddingApp._run_single: executing graph via graphExecute")
        t_exec = time.time()
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
            logger.error(f"SimpleQnnEmbeddingApp._run_single: graphExecute failed with status code {rc}")
            raise RuntimeError(f"graphExecute failed rc={rc}")
        logger.debug(f"SimpleQnnEmbeddingApp._run_single: graphExecute succeeded in {time.time() - t_exec:.4f}s")

        dump_tensors(outputs, n_out, kind="OUTPUTS_POST_EXEC")

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
