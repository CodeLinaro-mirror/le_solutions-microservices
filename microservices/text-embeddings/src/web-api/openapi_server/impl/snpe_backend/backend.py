# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

from __future__ import annotations

import os
import ctypes
from typing import List, Optional
from ctypes import (
    c_void_p,
    c_uint32,
    c_int32,
    c_uint8,
    c_uint64,
    c_size_t,
    c_char_p,
    POINTER,
)

from openapi_server.impl.embedding_backend import EmbeddingBackend
from openapi_server.impl.text_embed_qnn import ATTN_MASK_NAMES
from openapi_server.impl.snpe_backend.tokenizer import load_snpe_tokenizer, TokenizerAdapter

# --------------------------------------------------------------------------------------
# Helper functions
# --------------------------------------------------------------------------------------

def _prod(xs):
    p = 1
    for x in xs:
        p *= int(x)
    return int(p)


# --------------------------------------------------------------------------------------
# SNPE Backend Implementation
# --------------------------------------------------------------------------------------

class SimpleSnpeEmbeddingApp(EmbeddingBackend):
    """
    SNPE Deep Learning Container (.dlc) embedding backend.

    Implements :class:`EmbeddingBackend` so it can be used interchangeably
    with other backends.
    """

    def __init__(
        self,
        snpe_lib: str,
        model_path: str,
        runtime_name: str,
        perf_profile_name: str,
        pad_token: int,
        assume_attention_mask: bool,
        tokenizer: TokenizerAdapter,
    ):
        self.snpe_lib = snpe_lib
        self.model_path = model_path
        self.runtime_name = runtime_name.upper()
        self.perf_profile_name = perf_profile_name.lower()
        self.pad_token = pad_token
        self.assume_attention_mask = assume_attention_mask
        self.tokenizer = tokenizer

        # Load SNPE Shared Library
        try:
            self.lib = ctypes.CDLL(snpe_lib, mode=ctypes.RTLD_GLOBAL)
        except OSError as e:
            raise RuntimeError(f"Failed to load SNPE library '{snpe_lib}': {e}")

        self._bind_apis()

        # Initialize SNPE logging
        try:
            # SNPE_LOG_LEVEL_WARN = 2, SNPE_LOG_LEVEL_ERROR = 1
            self.Snpe_Util_InitializeLogging(1)
        except Exception:
            pass

        self.snpe_handle = c_void_p(None)

        # Build SNPE engine
        self._init_snpe()

    def _bind_apis(self):
        # 1. Error / Logging APIs
        self.Snpe_ErrorCode_GetLastErrorString = self.lib.Snpe_ErrorCode_GetLastErrorString
        self.Snpe_ErrorCode_GetLastErrorString.restype = c_char_p
        self.Snpe_ErrorCode_GetLastErrorString.argtypes = []

        try:
            self.Snpe_Util_GetLastError = self.lib.Snpe_Util_GetLastError
            self.Snpe_Util_GetLastError.restype = c_char_p
            self.Snpe_Util_GetLastError.argtypes = []
        except AttributeError:
            self.Snpe_Util_GetLastError = self.Snpe_ErrorCode_GetLastErrorString

        try:
            self.Snpe_Util_InitializeLogging = self.lib.Snpe_Util_InitializeLogging
            self.Snpe_Util_InitializeLogging.restype = c_int32
            self.Snpe_Util_InitializeLogging.argtypes = [c_int32]
        except AttributeError:
            pass

        # 2. DlContainer APIs
        self.Snpe_DlContainer_Open = self.lib.Snpe_DlContainer_Open
        self.Snpe_DlContainer_Open.restype = c_void_p
        self.Snpe_DlContainer_Open.argtypes = [c_char_p]

        self.Snpe_DlContainer_Delete = self.lib.Snpe_DlContainer_Delete
        self.Snpe_DlContainer_Delete.restype = c_uint32
        self.Snpe_DlContainer_Delete.argtypes = [c_void_p]

        # 3. RuntimeList APIs
        self.Snpe_RuntimeList_Create = self.lib.Snpe_RuntimeList_Create
        self.Snpe_RuntimeList_Create.restype = c_void_p
        self.Snpe_RuntimeList_Create.argtypes = []

        self.Snpe_RuntimeList_Add = self.lib.Snpe_RuntimeList_Add
        self.Snpe_RuntimeList_Add.restype = c_uint32
        self.Snpe_RuntimeList_Add.argtypes = [c_void_p, c_int32]

        self.Snpe_RuntimeList_Delete = self.lib.Snpe_RuntimeList_Delete
        self.Snpe_RuntimeList_Delete.restype = c_uint32
        self.Snpe_RuntimeList_Delete.argtypes = [c_void_p]

        # 4. SNPEBuilder APIs
        self.Snpe_SNPEBuilder_Create = self.lib.Snpe_SNPEBuilder_Create
        self.Snpe_SNPEBuilder_Create.restype = c_void_p
        self.Snpe_SNPEBuilder_Create.argtypes = [c_void_p]

        self.Snpe_SNPEBuilder_Delete = self.lib.Snpe_SNPEBuilder_Delete
        self.Snpe_SNPEBuilder_Delete.restype = c_uint32
        self.Snpe_SNPEBuilder_Delete.argtypes = [c_void_p]

        self.Snpe_SNPEBuilder_SetRuntimeProcessorOrder = self.lib.Snpe_SNPEBuilder_SetRuntimeProcessorOrder
        self.Snpe_SNPEBuilder_SetRuntimeProcessorOrder.restype = c_uint32
        self.Snpe_SNPEBuilder_SetRuntimeProcessorOrder.argtypes = [c_void_p, c_void_p]

        self.Snpe_SNPEBuilder_SetPerformanceProfile = self.lib.Snpe_SNPEBuilder_SetPerformanceProfile
        self.Snpe_SNPEBuilder_SetPerformanceProfile.restype = c_uint32
        self.Snpe_SNPEBuilder_SetPerformanceProfile.argtypes = [c_void_p, c_int32]

        self.Snpe_SNPEBuilder_Build = self.lib.Snpe_SNPEBuilder_Build
        self.Snpe_SNPEBuilder_Build.restype = c_void_p
        self.Snpe_SNPEBuilder_Build.argtypes = [c_void_p]

        # 5. SNPE Core APIs
        self.Snpe_SNPE_Delete = self.lib.Snpe_SNPE_Delete
        self.Snpe_SNPE_Delete.restype = c_uint32
        self.Snpe_SNPE_Delete.argtypes = [c_void_p]

        self.Snpe_SNPE_GetInputTensorNames = self.lib.Snpe_SNPE_GetInputTensorNames
        self.Snpe_SNPE_GetInputTensorNames.restype = c_void_p
        self.Snpe_SNPE_GetInputTensorNames.argtypes = [c_void_p]

        self.Snpe_SNPE_GetOutputTensorNames = self.lib.Snpe_SNPE_GetOutputTensorNames
        self.Snpe_SNPE_GetOutputTensorNames.restype = c_void_p
        self.Snpe_SNPE_GetOutputTensorNames.argtypes = [c_void_p]

        self.Snpe_SNPE_GetInputDimensions = self.lib.Snpe_SNPE_GetInputDimensions
        self.Snpe_SNPE_GetInputDimensions.restype = c_void_p
        self.Snpe_SNPE_GetInputDimensions.argtypes = [c_void_p, c_char_p]

        self.Snpe_SNPE_ExecuteITensors = self.lib.Snpe_SNPE_ExecuteITensors
        self.Snpe_SNPE_ExecuteITensors.restype = c_uint32
        self.Snpe_SNPE_ExecuteITensors.argtypes = [c_void_p, c_void_p, c_void_p]

        # 6. StringList APIs
        self.Snpe_StringList_Size = self.lib.Snpe_StringList_Size
        self.Snpe_StringList_Size.restype = c_size_t
        self.Snpe_StringList_Size.argtypes = [c_void_p]

        self.Snpe_StringList_At = self.lib.Snpe_StringList_At
        self.Snpe_StringList_At.restype = c_char_p
        self.Snpe_StringList_At.argtypes = [c_void_p, c_size_t]

        self.Snpe_StringList_Delete = self.lib.Snpe_StringList_Delete
        self.Snpe_StringList_Delete.restype = c_uint32
        self.Snpe_StringList_Delete.argtypes = [c_void_p]

        # 7. TensorShape APIs
        self.Snpe_TensorShape_Rank = self.lib.Snpe_TensorShape_Rank
        self.Snpe_TensorShape_Rank.restype = c_size_t
        self.Snpe_TensorShape_Rank.argtypes = [c_void_p]

        self.Snpe_TensorShape_GetDimensions = self.lib.Snpe_TensorShape_GetDimensions
        self.Snpe_TensorShape_GetDimensions.restype = POINTER(c_size_t)
        self.Snpe_TensorShape_GetDimensions.argtypes = [c_void_p]

        self.Snpe_TensorShape_Delete = self.lib.Snpe_TensorShape_Delete
        self.Snpe_TensorShape_Delete.restype = c_uint32
        self.Snpe_TensorShape_Delete.argtypes = [c_void_p]

        # 8. ITensor APIs
        self.Snpe_Util_CreateITensor = self.lib.Snpe_Util_CreateITensor
        self.Snpe_Util_CreateITensor.restype = c_void_p
        self.Snpe_Util_CreateITensor.argtypes = [c_void_p]

        self.Snpe_ITensor_GetData = self.lib.Snpe_ITensor_GetData
        self.Snpe_ITensor_GetData.restype = c_void_p
        self.Snpe_ITensor_GetData.argtypes = [c_void_p]

        self.Snpe_ITensor_GetSize = self.lib.Snpe_ITensor_GetSize
        self.Snpe_ITensor_GetSize.restype = c_size_t
        self.Snpe_ITensor_GetSize.argtypes = [c_void_p]

        self.Snpe_ITensor_Delete = self.lib.Snpe_ITensor_Delete
        self.Snpe_ITensor_Delete.restype = c_uint32
        self.Snpe_ITensor_Delete.argtypes = [c_void_p]

        # 9. TensorMap APIs
        self.Snpe_TensorMap_Create = self.lib.Snpe_TensorMap_Create
        self.Snpe_TensorMap_Create.restype = c_void_p
        self.Snpe_TensorMap_Create.argtypes = []

        self.Snpe_TensorMap_Delete = self.lib.Snpe_TensorMap_Delete
        self.Snpe_TensorMap_Delete.restype = c_uint32
        self.Snpe_TensorMap_Delete.argtypes = [c_void_p]

        self.Snpe_TensorMap_Add = self.lib.Snpe_TensorMap_Add
        self.Snpe_TensorMap_Add.restype = None
        self.Snpe_TensorMap_Add.argtypes = [c_void_p, c_char_p, c_void_p]

        self.Snpe_TensorMap_GetTensor_Ref = self.lib.Snpe_TensorMap_GetTensor_Ref
        self.Snpe_TensorMap_GetTensor_Ref.restype = c_void_p
        self.Snpe_TensorMap_GetTensor_Ref.argtypes = [c_void_p, c_char_p]

    def _get_last_error(self) -> str:
        err = self.Snpe_Util_GetLastError()
        return err.decode(errors="replace") if err else "Unknown SNPE error"

    def _init_snpe(self):
        # 1. Open DLC container
        container = self.Snpe_DlContainer_Open(self.model_path.encode("utf-8"))
        if not container:
            raise RuntimeError(f"Failed to open DLC container at {self.model_path}. Error: {self._get_last_error()}")

        # 2. Create SNPE Builder
        builder = self.Snpe_SNPEBuilder_Create(container)
        if not builder:
            self.Snpe_DlContainer_Delete(container)
            raise RuntimeError(f"Failed to create SNPE builder. Error: {self._get_last_error()}")

        # 3. Setup Runtime Order List
        runtime_list = self.Snpe_RuntimeList_Create()

        # SNPE_RUNTIME_DSP_FIXED8_TF = 2, SNPE_RUNTIME_GPU = 1, SNPE_RUNTIME_CPU = 0
        runtime_map = {
            "DSP": 2,
            "GPU": 1,
            "CPU": 0
        }
        runtime_val = runtime_map.get(self.runtime_name, 2) # default to DSP
        self.Snpe_RuntimeList_Add(runtime_list, runtime_val)
        # Always fallback to CPU
        if runtime_val != 0:
            self.Snpe_RuntimeList_Add(runtime_list, 0)

        self.Snpe_SNPEBuilder_SetRuntimeProcessorOrder(builder, runtime_list)

        # 4. Setup Performance Profile
        # SNPE_PERFORMANCE_PROFILE_BALANCED = 0, HIGH_PERFORMANCE = 1, SUSTAINED_HIGH_PERFORMANCE = 4, POWER_SAVER = 2, BURST = 5
        perf_profile_map = {
            "balanced": 0,
            "high_performance": 1,
            "sustained_high_performance": 4,
            "power_saver": 2,
            "burst": 5
        }
        perf_val = perf_profile_map.get(self.perf_profile_name, 0)
        self.Snpe_SNPEBuilder_SetPerformanceProfile(builder, perf_val)

        # 5. Build SNPE Engine
        self.snpe_handle = self.Snpe_SNPEBuilder_Build(builder)

        # Cleanup builder structures
        self.Snpe_RuntimeList_Delete(runtime_list)
        self.Snpe_SNPEBuilder_Delete(builder)
        self.Snpe_DlContainer_Delete(container)

        if not self.snpe_handle:
            raise RuntimeError(f"Failed to build SNPE engine. Error: {self._get_last_error()}")

        # 6. Retrieve and cache Input/Output Tensor Names
        self._cache_io_names()

    def _cache_io_names(self):
        # Cache Inputs
        in_list = self.Snpe_SNPE_GetInputTensorNames(self.snpe_handle)
        in_size = self.Snpe_StringList_Size(in_list)
        self.input_names = [self.Snpe_StringList_At(in_list, i).decode(errors="replace") for i in range(in_size)]
        self.Snpe_StringList_Delete(in_list)

        # Cache Outputs
        out_list = self.Snpe_SNPE_GetOutputTensorNames(self.snpe_handle)
        out_size = self.Snpe_StringList_Size(out_list)
        self.output_names = [self.Snpe_StringList_At(out_list, i).decode(errors="replace") for i in range(out_size)]
        self.Snpe_StringList_Delete(out_list)

        # Auto-resolve main input and output names (same fallback fallback logic as QNN)
        from openapi_server.impl.text_embed_qnn import TOKEN_NAMES_FALLBACK
        self.resolved_input_name = next(
            (cand for cand in TOKEN_NAMES_FALLBACK if cand in self.input_names),
            self.input_names[0] if self.input_names else None
        )
        self.resolved_output_name = "embeddings" if "embeddings" in self.output_names else (self.output_names[0] if self.output_names else None)

        if not self.resolved_input_name or not self.resolved_output_name:
            raise RuntimeError(f"Failed to resolve input/output tensors. Inputs: {self.input_names}, Outputs: {self.output_names}")

    @classmethod
    def from_model_path(cls, model_path: str):
        snpe_lib = os.getenv("SNPE_LIB_PATH", "libSNPE.so")
        runtime = os.getenv("SNPE_RUNTIME", "DSP")
        perf_profile = os.getenv("SNPE_PERF_PROFILE", "balanced")
        pad_token = int(os.getenv("SNPE_PAD_TOKEN", "0"))

        tokenizer = load_snpe_tokenizer()

        return cls(
            snpe_lib=snpe_lib,
            model_path=model_path,
            runtime_name=runtime,
            perf_profile_name=perf_profile,
            pad_token=pad_token,
            assume_attention_mask=True,
            tokenizer=tokenizer
        )

    # ------------------------------------------------------------------
    # EmbeddingBackend interface
    # ------------------------------------------------------------------

    def embed_texts(self, texts: List[str]) -> List[List[float]]:
        if self.tokenizer is None:
            raise RuntimeError("Tokenizer not available.")

        vectors = []
        for text in texts:
            tokens = self.tokenizer.encode(text)
            vec = self._run_single(tokens)
            vectors.append(vec)

        return vectors

    def encode_tokens(self, text: str) -> List[int]:
        if self.tokenizer is None:
            raise RuntimeError("Tokenizer not available.")
        return self.tokenizer.encode(text)

    def close(self):
        if self.snpe_handle:
            try:
                self.Snpe_SNPE_Delete(self.snpe_handle)
            except Exception:
                pass
            self.snpe_handle = None

    def _run_single(self, tokens: List[int]) -> List[float]:
        input_map = self.Snpe_TensorMap_Create()
        output_map = self.Snpe_TensorMap_Create()
        keepalive_tensors = []

        # 1. Create and populate input ITensors
        for name in self.input_names:
            shape = self.Snpe_SNPE_GetInputDimensions(self.snpe_handle, name.encode("utf-8"))
            rank = self.Snpe_TensorShape_Rank(shape)
            dims_ptr = self.Snpe_TensorShape_GetDimensions(shape)

            n_elem = 1
            for r in range(rank):
                n_elem *= dims_ptr[r]

            itensor = self.Snpe_Util_CreateITensor(shape)
            keepalive_tensors.append(itensor)

            data_ptr = self.Snpe_ITensor_GetData(itensor)
            float_arr = (ctypes.c_float * n_elem).from_address(data_ptr)

            if name == self.resolved_input_name:
                for j, tok in enumerate(tokens[:n_elem]):
                    float_arr[j] = float(tok)
                for j in range(len(tokens[:n_elem]), n_elem):
                    float_arr[j] = float(self.pad_token)
            elif name.lower() in ATTN_MASK_NAMES and self.assume_attention_mask:
                for j in range(n_elem):
                    float_arr[j] = 1.0 if j < len(tokens) else 0.0
            else:
                for j in range(n_elem):
                    float_arr[j] = 0.0

            self.Snpe_TensorMap_Add(input_map, name.encode("utf-8"), itensor)
            self.Snpe_TensorShape_Delete(shape)

        # 2. Execute inference
        rc = self.Snpe_SNPE_ExecuteITensors(self.snpe_handle, input_map, output_map)
        if rc != 0:
            # Cleanup input structures on failure
            self.Snpe_TensorMap_Delete(input_map)
            self.Snpe_TensorMap_Delete(output_map)
            for t in keepalive_tensors:
                self.Snpe_ITensor_Delete(t)
            raise RuntimeError(f"Snpe_SNPE_ExecuteITensors failed with rc={rc}. Error: {self._get_last_error()}")

        # 3. Extract outputs
        out_tensor = self.Snpe_TensorMap_GetTensor_Ref(output_map, self.resolved_output_name.encode("utf-8"))
        if not out_tensor:
            self.Snpe_TensorMap_Delete(input_map)
            self.Snpe_TensorMap_Delete(output_map)
            for t in keepalive_tensors:
                self.Snpe_ITensor_Delete(t)
            raise RuntimeError(f"Output tensor '{self.resolved_output_name}' not found in execution outputs.")

        out_size = self.Snpe_ITensor_GetSize(out_tensor)
        out_ptr = self.Snpe_ITensor_GetData(out_tensor)

        float_arr_out = (ctypes.c_float * out_size).from_address(out_ptr)
        output_vector = list(float_out for float_out in float_arr_out)

        # 4. Cleanup maps & keepalive input tensors
        self.Snpe_TensorMap_Delete(input_map)
        self.Snpe_TensorMap_Delete(output_map)
        for t in keepalive_tensors:
            self.Snpe_ITensor_Delete(t)

        return output_vector
