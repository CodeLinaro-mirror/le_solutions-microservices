# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

import ctypes
import numpy as np
from typing import Dict

from .qnn_types import Qnn_Tensor_t, qnn_dtype_size_bytes  # <-- use suffix-based fallback

# QNN data types (legacy app-level enums used elsewhere in the code)
QNN_DATATYPE_FLOAT_32 = 0
QNN_DATATYPE_UINT_8   = 1
QNN_DATATYPE_UINT_16  = 2
QNN_DATATYPE_UINT_32  = 3
QNN_DATATYPE_UINT_64  = 4
QNN_DATATYPE_INT_8    = 5
QNN_DATATYPE_INT_16   = 6
QNN_DATATYPE_INT_32   = 7
QNN_DATATYPE_INT_64   = 8
QNN_DATATYPE_BOOL_8   = 9
QNN_DATATYPE_UFIXED_8 = 10
QNN_DATATYPE_UFIXED_16= 11

# Bytes per element for the legacy (app-level) enum space above.
# For real tensors coming from QNN, dataType is bit-coded (e.g., 0x..16/0x..32/0x..64).
# When those appear, we will fall back to qnn_dtype_size_bytes().
DATA_TYPE_SIZE: Dict[int, int] = {
    QNN_DATATYPE_FLOAT_32: 4,
    QNN_DATATYPE_UINT_8:   1,
    QNN_DATATYPE_UINT_16:  2,
    QNN_DATATYPE_UINT_32:  4,
    QNN_DATATYPE_UINT_64:  8,
    QNN_DATATYPE_INT_8:    1,
    QNN_DATATYPE_INT_16:   2,
    QNN_DATATYPE_INT_32:   4,
    QNN_DATATYPE_INT_64:   8,
    QNN_DATATYPE_BOOL_8:   1,
    QNN_DATATYPE_UFIXED_8: 1,
    QNN_DATATYPE_UFIXED_16:2,
}

class GraphInfo(ctypes.Structure):
    _fields_ = [
        ('graph',            ctypes.c_void_p),
        ('graphName',        ctypes.c_char_p),
        ('inputTensors',     ctypes.POINTER(Qnn_Tensor_t)),
        ('numInputTensors',  ctypes.c_uint32),
        ('outputTensors',    ctypes.POINTER(Qnn_Tensor_t)),
        ('numOutputTensors', ctypes.c_uint32),
    ]

def element_count(dim_ptr, rank: int) -> int:
    """
    Safe element count:
    - If rank == 0, treat as scalar (1).
    - If rank > 0 and dim_ptr is NULL, raise a clear error instead of segfaulting.
    """
    if rank <= 0:
        return 1  # many QNN graphs use rank=0 for scalar tensors
    if not bool(dim_ptr):
        print("Tensor dimensions pointer is NULL while rank > 0")
        raise RuntimeError("Tensor dimensions pointer is NULL while rank > 0")

    dims = [dim_ptr[i] for i in range(rank)]
    if any(d <= 0 for d in dims):
        print(f"Invalid dimension(s): {dims}")
        raise RuntimeError(f"Invalid dimension(s): {dims}")
    return int(np.prod(dims))

def calc_length_bytes(t) -> int:
    """
    Compute buffer length = element_count * dtype_size, with defensive defaults.

    IMPORTANT:
    - First try the legacy DATA_TYPE_SIZE map (used by a small subset of app enums).
    - If that doesn't recognize the dtype (typical for QNN bit-coded types),
      fall back to qnn_dtype_size_bytes(), which derives byte-size from the
      low-byte suffix: 0x..08 -> 1, 0x..16 -> 2, 0x..32 -> 4, 0x..64 -> 8.
    """
    rank = int(getattr(t, "rank", 0))
    elems = element_count(getattr(t, "dimensions", None), rank)

    dt_code = int(getattr(t, "dataType", 0))
    dtype_size = DATA_TYPE_SIZE.get(dt_code)
    if dtype_size is None:
        # Use the bit-suffix fallback for real QNN dtype codes (e.g., 0x0416)
        dtype_size = int(qnn_dtype_size_bytes(dt_code))

    return int(elems * dtype_size)
